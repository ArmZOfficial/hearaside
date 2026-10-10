#include "HubProcessor.h"
#include "HubEditor.h"
#include "Strings.h"

#include "ssdsp/dsp.h"

namespace hearaside {

using ssengine::HubEngine;

namespace {

juce::String dbText(float v, int) {
    if (v <= ssdsp::kMinusInfDb) return "-inf dB";
    return juce::String(v, 1) + " dB";
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout HubProcessor::createLayout() {
    using namespace juce;
    auto dbAttr = AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(dbText);
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { hubparam::Master, 1 }, "Stream Level",
        NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f, dbAttr));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { hubparam::LimiterOn, 1 }, "Peak Protection", true));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { hubparam::Ceiling, 1 }, "Peak Ceiling",
        NormalisableRange<float>(-12.0f, 0.0f, 0.1f), -1.0f, dbAttr));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { hubparam::Preview, 1 }, "Hear Viewers Mix", false));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { hubparam::Panic, 1 }, "Mute Stream", false));
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { hubparam::SyncSafety, 1 }, "Sync Safety",
        StringArray { "0", "1 block", "2 blocks" }, 0));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { hubparam::Headphones, 1 }, "Headphone Master",
        NormalisableRange<float>(-60.0f, 6.0f, 0.1f), 0.0f, dbAttr));
    return { p.begin(), p.end() };
}

HubProcessor::HubProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "HEARASIDE_HUB", createLayout()) {
    master_     = apvts_.getRawParameterValue(hubparam::Master);
    limiter_    = apvts_.getRawParameterValue(hubparam::LimiterOn);
    ceiling_    = apvts_.getRawParameterValue(hubparam::Ceiling);
    preview_    = apvts_.getRawParameterValue(hubparam::Preview);
    panic_      = apvts_.getRawParameterValue(hubparam::Panic);
    sync_       = apvts_.getRawParameterValue(hubparam::SyncSafety);
    headphones_ = apvts_.getRawParameterValue(hubparam::Headphones);
    headphoneGain_.reset(48000.0, 0.020);
    headphoneGain_.setCurrentAndTargetValue(1.0f);

    lastConnect_ = juce::Time::getMillisecondCounter();
    engine_.connect(busName_.toStdString());
    engine_.prepare(sampleRate_, maxBlock_);
    pushStemNames();
    startTimerHz(20);
    latencyMeter_.startThread(juce::Thread::Priority::low);
}

HubProcessor::~HubProcessor() {
    control_.stop();
    share_.stop();
    muteAllBut(-1, -1);   // never leave Tracks muted behind
    latencyMeter_.stopThread(3000);
    stopTimer();
    engine_.disconnect();
}

bool HubProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
}

void HubProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock_ = juce::jmax(16, samplesPerBlock);
    engine_.prepare(sampleRate_, maxBlock_);
    headphoneGain_.reset(sampleRate_, 0.020);
    headphoneGain_.setCurrentAndTargetValue(ssdsp::dbToGain(headphones_->load()));
    setLatencySamples(0);   // the master output itself is never delayed
}

void HubProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) { run(b, false); }
void HubProcessor::processBlockBypassed(juce::AudioBuffer<float>& b, juce::MidiBuffer&) { run(b, true); }

void HubProcessor::run(juce::AudioBuffer<float>& buffer, bool bypassed) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0) return;
    const int numCh = juce::jmin(buffer.getNumChannels(), 2);

    int64_t time = ssengine::kNoTime;
    bool playing = false;
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition()) {
            if (const auto t = pos->getTimeInSamples()) time = *t;
            playing = pos->getIsPlaying();
        }
    }

    ssengine::HubParams hp;
    hp.masterDb = master_->load();
    hp.limiterOn = limiter_->load() > 0.5f;
    hp.ceilingDb = ceiling_->load();
    // auto sync silences the stream: "Hear viewers' mix" would then silence the headphones too, and the
    // mic, which measures by hearing them, would hear nothing
    hp.silence = silence_.load(std::memory_order_relaxed);
    hp.preview = preview_->load() > 0.5f && !hp.silence;
    hp.panic = panic_->load() > 0.5f;
    hp.syncSafety = juce::roundToInt(sync_->load());
    hp.bypassed = bypassed;
    const bool offline = isNonRealtime();
    engine_.process(buffer.getArrayOfWritePointers(), numCh, n, hp, time, playing, offline);
    if (bypassed || engine_.role() != HubEngine::Role::Owner) return;   // extra Hubs build the viewers FX channel

    // Headphone master: only what you hear. The Stream Mix was already written above and exports
    // (offline) are never attenuated.
    headphoneGain_.setTargetValue(offline ? 1.0f : ssdsp::dbToGain(headphones_->load()));
    if (headphoneGain_.isSmoothing()) {
        for (int i = 0; i < n; ++i) {
            const float g = headphoneGain_.getNextValue();
            for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.getWritePointer(c)[i] *= g;
        }
    } else if (const float g = headphoneGain_.getTargetValue(); g != 1.0f) {
        if (g == 0.0f) buffer.clear(); else buffer.applyGain(g);
    }
}

juce::AudioProcessorEditor* HubProcessor::createEditor() { return new HubEditor(*this); }

// ---------------------------------------------------------------------------------------------

void HubProcessor::setBusName(const juce::String& name) {
    const auto b = name.trim().isEmpty() ? juce::String("Main") : name.trim();
    if (b == busName_) return;
    busName_ = b;
    lastConnect_ = juce::Time::getMillisecondCounter();
    engine_.connect(busName_.toStdString());
    pushStemNames();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

std::vector<TrackView> HubProcessor::tracks() const {
    std::vector<TrackView> out;
    auto* bus = engine_.bus();
    if (!bus) return out;
    const uint64_t now = ssbus::nowNs();
    for (int i = 0; i < ssbus::kMaxSlots; ++i) {
        const auto& s = bus->slots[i];
        if (s.state.load(std::memory_order_acquire) != ssbus::kSlotActive) continue;
        if (s.flags.load(std::memory_order_acquire) & ssbus::kFlagApp) continue;   // listed with its App Audio
        TrackView v;
        v.slot = i;
        v.sequence = s.sequence.load(std::memory_order_relaxed);
        std::string name, uuid;
        uint32_t colour = 0;
        ssbus::readSlotIdentity(s, name, uuid, colour);
        v.name = juce::String::fromUTF8(name.c_str());
        v.uuid = juce::String(uuid);
        v.colourARGB = colour;
        const uint32_t f = s.flags.load(std::memory_order_acquire);
        v.mon = (f & ssbus::kFlagMon) != 0;
        v.str = (f & ssbus::kFlagStr) != 0;
        v.solo = (f & ssbus::kFlagSolo) != 0;
        v.bypassed = (f & ssbus::kFlagBypassed) != 0;
        v.mono = (f & ssbus::kFlagMono) != 0;
        v.gainDb = ssbus::bitsFloat(s.strGainBits.load(std::memory_order_relaxed));
        v.pan = ssbus::bitsFloat(s.strPanBits.load(std::memory_order_relaxed));
        v.delayMs = ssbus::bitsFloat(s.strDelayBits.load(std::memory_order_relaxed));
        v.trimDb = ssbus::bitsFloat(s.monTrimBits.load(std::memory_order_relaxed));
        v.stem = s.stemIndex.load(std::memory_order_relaxed);
        v.chainMs = ssbus::bitsFloat(s.chainLatencyBits.load(std::memory_order_relaxed));
        v.peakIn = juce::jmax(ssbus::bitsFloat(s.peakInBits[0].load(std::memory_order_relaxed)),
                              ssbus::bitsFloat(s.peakInBits[1].load(std::memory_order_relaxed)));
        v.peakStream = juce::jmax(ssbus::bitsFloat(s.peakStreamBits[0].load(std::memory_order_relaxed)),
                                  ssbus::bitsFloat(s.peakStreamBits[1].load(std::memory_order_relaxed)));
        v.hubStatus = s.hubStatus.load(std::memory_order_relaxed);
        const uint64_t hb = s.heartbeatNs.load(std::memory_order_relaxed);
        v.active = hb != 0 && now - hb < 1000000000ull;
        if (v.name.isEmpty()) v.name = tr(Str::TrackWord) + " " + juce::String(i + 1);
        out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const TrackView& a, const TrackView& b) { return a.sequence < b.sequence; });
    return out;
}

std::vector<SourceView> HubProcessor::sources() const {
    std::vector<SourceView> out;
    auto* bus = engine_.bus();
    if (!bus) return out;
    const uint64_t now = ssbus::nowNs();
    for (int i = 0; i < ssbus::kMaxSources; ++i) {
        const auto& s = bus->sources[i];
        if (s.state.load(std::memory_order_acquire) != ssbus::kSlotActive) continue;
        SourceView v;
        v.index = i;
        v.sequence = s.sequence.load(std::memory_order_relaxed);
        std::string name, app;
        ssbus::readSourceIdentity(s, name, app, v.colourARGB);
        v.name = juce::String::fromUTF8(name.c_str());
        v.app = juce::String::fromUTF8(app.c_str());
        v.flags = s.flags.load(std::memory_order_relaxed);
        v.capture = s.capture.load(std::memory_order_relaxed);
        v.levelDb = ssbus::bitsFloat(s.levelBits.load(std::memory_order_relaxed));
        v.peak = ssbus::bitsFloat(s.peakBits.load(std::memory_order_relaxed));
        v.latencyMs = ssbus::bitsFloat(s.latencyBits.load(std::memory_order_relaxed));
        v.recordSec = s.recordMs.load(std::memory_order_relaxed) * 0.001;
        v.delayMs = ssbus::bitsFloat(s.delayBits.load(std::memory_order_relaxed));
        if (uint32_t fid = 0; FriendDirectory::isFriendApp(v.app, &fid)) {
            FriendDirectory::Info fi;
            if (FriendDirectory::find(fid, fi) && fi.delayMs >= 0.0f) v.takeShiftMs = fi.delayMs + v.latencyMs;
        }
        const uint64_t hb = s.heartbeatNs.load(std::memory_order_relaxed);
        v.active = hb != 0 && now - hb < 1000000000ull;
        if (v.name.isEmpty()) v.name = "App Audio " + juce::String(i + 1);
        if (const int t = s.trackSlot.load(std::memory_order_relaxed); t >= 0 && t < ssbus::kMaxSlots) {
            const auto& sl = bus->slots[t];
            const uint32_t f = sl.flags.load(std::memory_order_acquire);
            if (sl.state.load(std::memory_order_acquire) == ssbus::kSlotActive && (f & ssbus::kFlagApp)) {
                v.slot = t;
                v.mon = (f & ssbus::kFlagMon) != 0;
                v.str = (f & ssbus::kFlagStr) != 0;
                v.gainDb = ssbus::bitsFloat(sl.strGainBits.load(std::memory_order_relaxed));
                v.trimDb = ssbus::bitsFloat(sl.monTrimBits.load(std::memory_order_relaxed));
            }
        }
        out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const SourceView& a, const SourceView& b) { return a.sequence < b.sequence; });
    return out;
}

void HubProcessor::sendSource(int index, ssbus::SourceParam id, float value) {
    if (auto* bus = engine_.bus(); bus && index >= 0 && index < ssbus::kMaxSources)
        ssbus::postSourceCommand(bus->sources[index], id, value);
}

void HubProcessor::chooseSourceApp(int index, const juce::String& exe) {
    if (auto* bus = engine_.bus(); bus && index >= 0 && index < ssbus::kMaxSources)
        ssbus::requestSourceApp(bus->sources[index], exe.toStdString());
}

void HubProcessor::send(int slot, ssbus::ParamId id, float value) {
    if (auto* bus = engine_.bus(); bus && slot >= 0 && slot < ssbus::kMaxSlots)
        ssbus::postCommand(bus->slots[slot], id, value);
}

void HubProcessor::rename(int slot, const juce::String& name) {
    if (auto* bus = engine_.bus(); bus && slot >= 0 && slot < ssbus::kMaxSlots)
        ssbus::requestRename(bus->slots[slot], name.trim().toStdString());
}

void HubProcessor::setParam(const char* id, float plain) {
    if (auto* p = apvts_.getParameter(id)) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(plain));
        p->endChangeGesture();
    }
}

bool HubProcessor::obsConnected() const {
    auto* bus = engine_.bus();
    if (!bus) return false;
    const uint64_t hb = bus->header.consumerHeartbeatNs.load(std::memory_order_relaxed);
    return hb != 0 && ssbus::nowNs() - hb < 1500000000ull;
}

double HubProcessor::latencyToObsMs() const {
    auto* bus = engine_.bus();
    if (!bus) return 0.0;
    const double sr = juce::jmax(8000.0, sampleRate_);
    const double frames = double(bus->header.hubLatencyFrames.load(std::memory_order_relaxed))
                        + double(bus->header.hubBlockSize.load(std::memory_order_relaxed));
    return frames * 1000.0 / sr + double(bus->header.consumerBufferMs.load(std::memory_order_relaxed));
}

HubProcessor::LatencyInfo HubProcessor::latency() const {
    LatencyInfo li;
    auto* bus = engine_.bus();
    li.sampleRate = juce::jmax(8000.0, sampleRate_);
    if (!bus) return li;
    const double fpm = li.sampleRate * 0.001;
    li.block = int(bus->header.hubBlockSize.load(std::memory_order_relaxed));
    li.dawMs = li.block / fpm;
    li.hubMs = int(bus->header.hubLatencyFrames.load(std::memory_order_relaxed)) / fpm;
    li.masterFxMs = masterFxMs_.load(std::memory_order_relaxed);
    li.trackFxMs = trackFxMs_.load(std::memory_order_relaxed);
    li.obs = obsConnected();
    li.obsMs = li.obs ? double(bus->header.consumerBufferMs.load(std::memory_order_relaxed)) : 0.0;
    return li;
}

void HubProcessor::setStemName(int i, const juce::String& name) {
    stemNames_[size_t(juce::jlimit(0, ssbus::kMaxStems - 1, i))] = name.trim();
    pushStemNames();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

void HubProcessor::pushStemNames() {
    engine_.setOutputName(0, "Stream Mix");
    for (int i = 0; i < ssbus::kMaxStems; ++i)
        engine_.setOutputName(1 + i, (stemNames_[size_t(i)].isNotEmpty() ? stemNames_[size_t(i)] : "Stem " + juce::String(i + 1)).toStdString());
}

void HubProcessor::timerCallback() {
    const auto now = juce::Time::getMillisecondCounter();
    if (!connected() && now - lastConnect_ > 2000) {
        lastConnect_ = now;
        if (engine_.connect(busName_.toStdString())) { pushStemNames(); stateChanged.sendChangeMessage(); }
    }
    if (now - lastMaintain_ >= 500) {
        lastMaintain_ = now;
        const auto before = engine_.role();
        engine_.maintain();
        if (engine_.role() != before) { if (engine_.role() == HubEngine::Role::Owner) pushStemNames(); stateChanged.sendChangeMessage(); }
    }
    FriendDirectory::update(engine_.bus());
    serviceRemote();
    serviceRequests();
    serviceFeeders();
    serviceAutoSync();
    serviceShare();
    serviceControl();
    serviceSilence();
}

void HubProcessor::serviceSilence() {
    auto* bus = engine_.bus();
    if (bus == nullptr || engine_.role() != HubEngine::Role::Owner) { silentSince_ = 0; viewersSilent_ = false; return; }
    const auto& sh = bus->streamHeader;
    const float out = juce::jmax(ssbus::bitsFloat(sh.peakBits[0][0].load(std::memory_order_relaxed)),
                                 ssbus::bitsFloat(sh.peakBits[0][1].load(std::memory_order_relaxed)));
    juce::String feeding;   // a track meant for the viewers that has sound right now
    for (const auto& v : tracks())
        if (v.active && v.str && v.gainDb > -59.0f && v.peakIn > 0.003f) { feeding = v.name; break; }   // > -50 dBFS
    const bool panic = panic_->load() > 0.5f;   // muted on purpose: not a problem
    const auto now = juce::Time::getMillisecondCounter();
    if (out < 0.001f && feeding.isNotEmpty() && !panic) {   // < -60 dBFS out
        if (silentSince_ == 0) silentSince_ = now;
        silentTrack_ = feeding;
    } else {
        silentSince_ = 0;
    }
    const bool silent = silentSince_ != 0 && now - silentSince_ > 5000;
    if (silent != viewersSilent_) { viewersSilent_ = silent; stateChanged.sendChangeMessage(); }
}

juce::String HubProcessor::diagnostics() const {
    const juce::PluginHostType host;
    const auto li = latency();
    const auto ts = tracks();
    const auto ss = sources();
    juce::String r;
    r << "HEARASIDE " << HEARASIDE_VERSION << " - problem report " << juce::Time::getCurrentTime().toISO8601(true) << "\n"
      << "DAW: " << host.getHostDescription() << " (" << juce::File(host.getHostPath()).getFileName() << ")\n"
      << "OS: " << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " 64-bit" : "") << "\n"
      << "sample rate " << juce::String(li.sampleRate, 0) << " Hz, buffer " << li.block << " (" << juce::String(li.dawMs, 1) << " ms)\n"
      << "Hub: " << (engine_.role() == HubEngine::Role::Owner ? "owner" : engine_.role() == HubEngine::Role::Secondary ? "second Hub (pass-through)" : "not connected")
      << ", bus \"" << busName_ << "\", stream muted " << (panic_->load() > 0.5f ? "yes" : "no") << ", preview " << (preview_->load() > 0.5f ? "yes" : "no")
      << ", limiter " << (limiter_->load() > 0.5f ? "on" : "off") << "\n"
      << "OBS: " << (li.obs ? "connected, delay to OBS " + juce::String(li.total(), 1) + " ms (track fx " + juce::String(li.trackFxMs, 1)
                             + ", master fx " + juce::String(li.masterFxMs, 1) + ", OBS " + juce::String(li.obsMs, 1) + ")" : juce::String("not connected")) << "\n"
      << "viewers silent: " << (viewersSilent_ ? "YES" : "no") << "\n"
      << "tracks: " << ts.size() << "\n";
    for (const auto& v : ts)
        r << "  [" << v.slot << "] you " << (v.mon ? "on" : "off") << ", viewers " << (v.str ? "on" : "off") << ", " << juce::String(v.gainDb, 1)
          << " dB, delay " << juce::String(v.delayMs, 1) << " ms" << (v.active ? "" : ", inactive") << (v.bypassed ? ", BYPASSED" : "")
          << ((v.hubStatus & ssbus::kHubStatusRateMismatch) ? ", RATE MISMATCH" : "") << ((v.hubStatus & ssbus::kHubStatusAhead) ? ", processed ahead" : "") << "\n";
    r << "App Audio: " << ss.size() << "\n";
    for (const auto& s : ss)
        r << "  [" << s.index << "] " << (s.on() ? "on" : "off") << ", capture state " << int(s.capture) << ", delay " << juce::String(s.delayMs, 1) << " ms\n";
    r << "auto sync: phase " << int(autoSync_.phase) << ", " << juce::String(autoSync_.deltaMs, 1) << " ms\n"
      << "sharing: " << (shareWanted_ ? "on" : "off") << ", tunnel " << int(share_.tunnel()) << ", permanent link "
      << (permanentLinksSet() ? juce::String(int(directory_.state())) : juce::String("not set")) << ", listeners " << share_.listeners() << "\n"
      << "REST API: " << (control_.running() ? "on, port " + juce::String(control_.port()) : juce::String("off")) << "\n";
    return r;   // track names, links, tokens and keys stay out on purpose
}

bool HubProcessor::permanentLinksSet() const { return settings_->permanentLinks() && settings_->shareBase().isNotEmpty(); }

// Permanent links live on the internet for months: projects from before them get 130-bit tokens
// from the OS's secure random source, and a secret that proves the links are theirs.
void HubProcessor::ensureStrongTokens() {
    if (listenToken_.length() < 24 || sendToken_.length() < 24) {
        const auto l = ShareServer::newToken(), s = ShareServer::newToken();
        if (l.isNotEmpty() && s.isNotEmpty()) { listenToken_ = l; sendToken_ = s; shareBus_ = nullptr; }   // restart with them
    }
    if (shareSecret_.isEmpty()) shareSecret_ = ShareServer::newSecret();
}

juce::String HubProcessor::permanentUrl(bool listen) const {
    if (!permanentLinksSet() || listenToken_.length() < 24) return {};
    return settings_->shareBase() + (listen ? "/l/" + listenToken_ : "/s/" + sendToken_);
}

void HubProcessor::serviceShare() {
    const bool permanent = permanentLinksSet();
    if (permanent) ensureStrongTokens();   // the link can be copied (and sent) before the first share
    // the share server follows the bus this Hub owns
    auto* bus = engine_.role() == HubEngine::Role::Owner ? engine_.bus() : nullptr;
    // the server runs for the listen links and/or for the friends' send-in links
    const bool serverWanted = shareWanted_ || friendRoom_.count() > 0;
    if (serverWanted && bus != shareBus_) {
        shareBus_ = bus;
        if (bus) share_.start(bus, listenToken_, sendToken_); else share_.stop();
        stateChanged.sendChangeMessage();
    } else if (!serverWanted && share_.running()) {
        share_.stop();
        shareBus_ = nullptr;
        stateChanged.sendChangeMessage();
    }
    share_.setListenEnabled(shareWanted_);
    friendRoom_.service(bus, engine_, share_, {});
    engine_.setLineUp(lineUp_, float(settings_->lineUpLimitMs()));
    if (!shareWanted_) return;
    // tell the share web site where the tunnel is (its own thread; heartbeat every 30 s)
    const auto base = permanent ? settings_->shareBase() : juce::String();
    share_.setAllowedOrigin(ShareDirectory::originOf(base));
    const bool ready = share_.running() && share_.tunnel() == ShareServer::Tunnel::Ready;
    directory_.update(base, listenToken_, sendToken_, shareSecret_, ready ? share_.publicBase() : juce::String());
}

void HubProcessor::setSharing(bool on) {
    if (on == shareWanted_) return;
    shareWanted_ = on;
    if (on) serviceShare();
    else directory_.stop();   // the permanent links say "not shared right now" at once; friends keep their server
    share_.setListenEnabled(on);
    stateChanged.sendChangeMessage();
}

uint32_t HubProcessor::addFriend(const juce::String& name) {
    const auto id = friendRoom_.add(name);
    if (id != 0) { serviceShare(); updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true)); stateChanged.sendChangeMessage(); }
    return id;
}

void HubProcessor::removeFriend(uint32_t id) {
    friendRoom_.remove(id);
    serviceShare();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void HubProcessor::renameFriend(uint32_t id, const juce::String& name) {
    friendRoom_.rename(id, name);
    serviceShare();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

juce::String HubProcessor::friendLink(uint32_t id) const {
    const auto t = friendRoom_.token(id);
    return t.isEmpty() || !share_.running() ? juce::String() : share_.friendUrl(t);
}

juce::String HubProcessor::newFriendLink(uint32_t id) {
    const auto slot = friendRoom_.slotOf(id);
    const auto t = friendRoom_.newLink(id);
    if (t.isEmpty()) return {};
    share_.kickFriend(slot);   // the old link is dead: the open page has to use the new one
    serviceShare();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
    return friendLink(id);
}

void HubProcessor::remeasureFriend(uint32_t id) { share_.remeasureFriend(friendRoom_.slotOf(id)); }

void HubProcessor::setLineUp(bool on) {
    if (on == lineUp_) return;
    lineUp_ = on;
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

juce::String HubProcessor::feederTrackName(int feeder) const {
    auto* bus = engine_.bus();
    if (bus == nullptr || feeder < 0 || feeder >= ssbus::kMaxFeeders) return {};
    std::string name;
    uint32_t colour = 0;
    return ssbus::readFeederIdentity(bus->feeders[feeder], name, colour) ? juce::String::fromUTF8(name.c_str()) : juce::String();
}

void HubProcessor::bringBackFriend(uint32_t id) {
    auto* bus = engine_.bus();
    const int slot = friendRoom_.slotOf(id);
    if (bus == nullptr || slot < 0) return;
    const int feeder = bus->friends[slot].feeder.load(std::memory_order_relaxed);
    if (feeder < 0 || feeder >= ssbus::kMaxFeeders) return;
    auto& f = bus->feeders[feeder];
    f.hubCommand.store(1, std::memory_order_relaxed);   // "go back to this track's own sound"
    f.hubCommandSeq.fetch_add(1, std::memory_order_release);
}

int HubProcessor::lineUpSlowestFriend() const {
    const int slot = engine_.lineUpSlowest();
    if (slot < 0) return 0;
    for (const auto& e : friendRoom_.entries()) if (e.slot == slot) return int(e.id);
    return 0;
}

void HubProcessor::measureLatency() {
    if (engine_.role() != HubEngine::Role::Owner) return;
    const auto r = engine_.measureLatencies();
    // steady number on screen: smooth small wobble, follow real changes (a plug-in added) at once
    auto settle = [](std::atomic<double>& v, double now) {
        if (now < 0.0) return;
        const double old = v.load(std::memory_order_relaxed);
        v.store(std::abs(now - old) > 2.0 ? now : old + 0.3 * (now - old), std::memory_order_relaxed);
    };
    settle(masterFxMs_, r.masterMs);
    settle(trackFxMs_, r.tracksMs);
}

// ---------------------------------------------------------------------------------------------
// remote control (OBS hotkeys)

void HubProcessor::serviceRemote() {
    auto* bus = engine_.bus();
    if (bus == nullptr || engine_.role() != HubEngine::Role::Owner) { remoteBus_ = nullptr; return; }
    if (bus != remoteBus_) {   // new bus or just became the owner: ignore commands queued before
        remoteBus_ = bus;
        remoteCursor_ = bus->header.remoteReserve.load(std::memory_order_acquire);
    }
    ssbus::pollRemote(*bus, remoteCursor_, [this](int target, uint32_t pid, float v) { applyRemote(target, pid, v); });
}

void HubProcessor::applyRemote(int target, uint32_t pid, float v) {
    using ssbus::RemoteParam;
    switch (static_cast<RemoteParam>(pid)) {
        case RemoteParam::Panic:        if (target < 0) setParam(hubparam::Panic, v > 0.5f ? 1.0f : 0.0f); break;
        case RemoteParam::Preview:      if (target < 0) setParam(hubparam::Preview, v > 0.5f ? 1.0f : 0.0f); break;
        case RemoteParam::Share:        if (target < 0) setSharing(v > 0.5f); break;
        case RemoteParam::AutoSync: {   // target = mic slot (-1 = guess), value = music slot, <= -2: App Audio -value-2
            const int r = juce::roundToInt(v);
            if (target < 0) startAutoSync();
            else startAutoSync(target, r <= -2 ? -r - 2 : r, r <= -2, 0);
            break;
        }
    }
}

void HubProcessor::serviceRequests() {
    auto* bus = engine_.bus();
    if (bus == nullptr || engine_.role() != HubEngine::Role::Owner) { requestBus_ = nullptr; return; }
    if (bus != requestBus_) {
        requestBus_ = bus;
        requestCursor_ = bus->requestReserve.load(std::memory_order_acquire);
    }
    ssbus::pollRequests(*bus, requestCursor_, [this, bus](int feeder, ssbus::RequestKind kind, uint32_t arg, const std::string& text) {
        if (feeder < 0 || feeder >= ssbus::kMaxFeeders) return;
        auto& f = bus->feeders[feeder];
        switch (kind) {
            case ssbus::kReqResolveToken: {
                juce::String tokenStr = juce::String::fromUTF8(text.c_str()).trim();
                const int sIdx = tokenStr.lastIndexOf("/s/");
                if (sIdx >= 0) tokenStr = tokenStr.substring(sIdx + 3);
                const int qIdx = tokenStr.indexOfChar('?');
                if (qIdx >= 0) tokenStr = tokenStr.substring(0, qIdx);
                const int hIdx = tokenStr.indexOfChar('#');
                if (hIdx >= 0) tokenStr = tokenStr.substring(0, hIdx);
                tokenStr = tokenStr.trim();

                uint32_t resolvedId = 0;
                for (const auto& entry : friendRoom_.entries()) {
                    if (entry.token == tokenStr) {
                        resolvedId = entry.id;
                        break;
                    }
                }
                ssbus::postReply(f, resolvedId != 0 ? resolvedId : ssbus::kReplyNotInRoom);
                break;
            }
            case ssbus::kReqCreateFriend: {
                if (friendRoom_.full()) {
                    ssbus::postReply(f, ssbus::kReplyRoomFull);
                } else {
                    juce::String name = juce::String::fromUTF8(text.c_str()).trim();
                    const uint32_t newId = addFriend(name);
                    ssbus::postReply(f, newId != 0 ? newId : ssbus::kReplyRoomFull);
                }
                break;
            }
            case ssbus::kReqRelease: {
                const int slot = friendRoom_.slotOf(arg);
                if (slot >= 0 && slot < ssbus::kMaxFriends) {
                    auto& h = bus->friends[slot];
                    if (h.feeder.load(std::memory_order_relaxed) == feeder) {
                        h.route.store(ssbus::kRouteDirect, std::memory_order_relaxed);
                        h.feeder.store(-1, std::memory_order_relaxed);
                        const int outSlot = h.outSlot.load(std::memory_order_relaxed);
                        if (outSlot >= 0 && outSlot < ssbus::kMaxSlots) {
                            if (bus->slots[outSlot].fedBy.load(std::memory_order_relaxed) == arg) {
                                bus->slots[outSlot].fedBy.store(0, std::memory_order_relaxed);
                                bus->slots[outSlot].fxLatencyBits.store(0, std::memory_order_relaxed);
                                bus->slots[outSlot].flags.fetch_and(~ssbus::kFlagViewersViaHub, std::memory_order_release);
                            }
                        }
                        h.outSlot.store(-1, std::memory_order_relaxed);
                    }
                }
                ssbus::postReply(f, 1);
                break;
            }
            case ssbus::kReqCopyLink: {
                const juce::String link = friendLink(arg);
                if (link.isNotEmpty()) {
                    juce::SystemClipboard::copyTextToClipboard(link);
                    ssbus::postReply(f, 1);
                } else {
                    ssbus::postReply(f, ssbus::kReplyFailed);
                }
                break;
            }
        }
    });
}

void HubProcessor::serviceFeeders() {
    auto* bus = engine_.bus();
    if (bus == nullptr || engine_.role() != HubEngine::Role::Owner) return;
    const uint64_t now = ssbus::nowNs();

    for (int j = 0; j < ssbus::kMaxFriends; ++j) {
        auto& fh = bus->friends[j];
        const uint32_t fid = fh.id.load(std::memory_order_acquire);
        if (fid == 0) continue;

        int curFeeder = fh.feeder.load(std::memory_order_acquire);
        if (curFeeder >= 0 && curFeeder < ssbus::kMaxFeeders) {
            auto& f = bus->feeders[curFeeder];
            if (f.state.load(std::memory_order_acquire) != ssbus::kSlotActive
                || f.friendId.load(std::memory_order_relaxed) != fid
                || now - f.heartbeatNs.load(std::memory_order_relaxed) > 800'000'000ull) {
                fh.feeder.store(-1, std::memory_order_release);
                fh.route.store(ssbus::kRouteDirect, std::memory_order_release);
                curFeeder = -1;
            }
        } else {
            curFeeder = -1;
        }

        if (curFeeder < 0) {
            for (int k = 0; k < ssbus::kMaxFeeders; ++k) {
                auto& f = bus->feeders[k];
                if (f.state.load(std::memory_order_acquire) == ssbus::kSlotActive
                    && f.friendId.load(std::memory_order_relaxed) == fid
                    && (now - f.heartbeatNs.load(std::memory_order_relaxed) < 800'000'000ull)) {
                    fh.feeder.store(k, std::memory_order_release);
                    curFeeder = k;
                    break;
                }
            }
        }

        if (curFeeder >= 0) {
            auto& f = bus->feeders[curFeeder];
            const bool flowing = f.status.load(std::memory_order_relaxed) == ssbus::kFeederFlowing;
            fh.route.store(flowing ? ssbus::kRouteDaw : ssbus::kRouteDirect, std::memory_order_release);
        } else {
            fh.route.store(ssbus::kRouteDirect, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// auto sync

void HubProcessor::muteAllBut(int slot, int source) {
    auto* bus = engine_.bus();
    if (!bus) return;
    const bool measuring = slot >= 0 || source >= 0;
    for (int i = 0; i < ssbus::kMaxSlots; ++i)
        bus->slots[i].hubMute.store(measuring && i != slot && bus->slots[i].state.load(std::memory_order_acquire) == ssbus::kSlotActive ? 1u : 0u,
                                    std::memory_order_relaxed);
    for (int i = 0; i < ssbus::kMaxSources; ++i)
        bus->sources[i].hubMute.store(measuring && i != source && bus->sources[i].state.load(std::memory_order_acquire) == ssbus::kSlotActive ? 1u : 0u,
                                      std::memory_order_relaxed);
}

bool HubProcessor::defaultSyncTracks(int& mic, int& ref, bool& refSource) const {
    const auto ts = tracks();
    const auto ss = sources();
    auto has = [](const juce::String& name, std::initializer_list<const char*> words) {
        for (const char* w : words) if (name.containsIgnoreCase(juce::String::fromUTF8(w))) return true;
        return false;
    };
    mic = -1;
    for (const auto& v : ts) if (mic < 0 && has(v.name, { "ร้อง", "ไมค์", "vocal", "vox", "mic", "voice" })) mic = v.slot;
    for (const auto& v : ts) if (mic < 0 && !v.mon && v.str) mic = v.slot;   // a live voice is usually not monitored through the DAW
    if (mic < 0 && !ts.empty()) mic = ts.front().slot;
    // the music: an App Audio that is capturing, else a track that sounds like music
    ref = -1;
    refSource = true;
    for (const auto& s : ss) if (ref < 0 && s.on() && s.capture == 2) ref = s.index;
    for (const auto& s : ss) if (ref < 0 && s.on()) ref = s.index;
    if (ref < 0) {
        refSource = false;
        for (const auto& v : ts)
            if (ref < 0 && v.slot != mic && has(v.name, { "เพลง", "ดนตรี", "music", "backing", "youtube", "app" })) ref = v.slot;
        for (const auto& v : ts) if (ref < 0 && v.slot != mic && v.str) ref = v.slot;
    }
    return mic >= 0 && ref >= 0 && (refSource || ref != mic);
}

void HubProcessor::startAutoSync() {
    int mic = -1, ref = -1;
    bool refSource = false;
    defaultSyncTracks(mic, ref, refSource);
    startAutoSync(mic, ref, refSource, 3000);
}

void HubProcessor::startAutoSync(int micSlot, int ref, bool refSource, int countdownMs) {
    auto* bus = engine_.bus();
    auto liveSlot = [bus](int s) { return bus && s >= 0 && s < ssbus::kMaxSlots && bus->slots[s].state.load() == ssbus::kSlotActive; };
    auto liveSource = [bus](int s) { return bus && s >= 0 && s < ssbus::kMaxSources && bus->sources[s].state.load() == ssbus::kSlotActive; };
    cancelAutoSync();
    autoSync_ = {};
    autoSync_.mic = micSlot;
    autoSync_.ref = ref;
    autoSync_.refSource = refSource;
    juce::String micName = "-", refName = "-";
    for (const auto& v : tracks()) {
        if (v.slot == micSlot) micName = v.name + (v.str ? "" : " (viewers off)");
        if (!refSource && v.slot == ref) refName = v.name;
    }
    for (const auto& s : sources())
        if (refSource && s.index == ref)
            refName = "App Audio " + s.name + " [" + s.app + "], capture ~" + juce::String(s.latencyMs, 0) + " ms";
    syncLog_ = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") + "  bus " + busName_ + ", " + juce::String(sampleRate_, 0) + " Hz, block "
             + juce::String(maxBlock_) + ", sync safety " + juce::String(sync_ ? int(sync_->load()) : 0) + "\n"
             + "mic: " + micName + "   music: " + refName + "\n";
    syncReads_.clear();
    if (!liveSlot(micSlot) || !(refSource ? liveSource(ref) : liveSlot(ref) && ref != micSlot) || engine_.role() != HubEngine::Role::Owner) {
        autoSync_.phase = SyncPhase::Failed;
        autoSync_.error = Str::SyncNeedTracks;
        syncWall_ = juce::Time::getMillisecondCounter();
        writeSyncLog(tr(Str::SyncNeedTracks));
        stateChanged.sendChangeMessage();
        return;
    }
    syncWall_ = juce::Time::getMillisecondCounter() + juce::uint32(juce::jmax(0, countdownMs));   // measuring starts then
    autoSync_.phase = SyncPhase::Countdown;
    stateChanged.sendChangeMessage();
    serviceAutoSync();
}

int HubProcessor::syncCountdown() const {
    if (autoSync_.phase != SyncPhase::Countdown) return 0;
    const auto now = juce::Time::getMillisecondCounter();
    return syncWall_ > now ? int((syncWall_ - now + 999) / 1000) : 0;
}

void HubProcessor::cancelAutoSync() {
    if (autoSync_.phase == SyncPhase::Reference || autoSync_.phase == SyncPhase::Microphone) {
        muteAllBut(-1, -1);
        silence_.store(false);
    }
    if (autoSync_.phase == SyncPhase::Countdown || autoSync_.phase == SyncPhase::Reference || autoSync_.phase == SyncPhase::Microphone)
        writeSyncLog("cancelled");
    autoSync_.phase = SyncPhase::Idle;
}

void HubProcessor::finishAutoSync(Str error) {
    muteAllBut(-1, -1);
    silence_.store(false);
    autoSync_.error = error;
    autoSync_.phase = error == Str::None ? SyncPhase::Done : SyncPhase::Failed;
    syncWall_ = juce::Time::getMillisecondCounter();
    writeSyncLog(error == Str::None ? "ok, vocal " + juce::String(autoSync_.deltaMs, 1) + " ms late (negative = early)" : tr(error));
    stateChanged.sendChangeMessage();
}

void HubProcessor::writeSyncLog(const juce::String& result) {
    if (syncLog_.isEmpty()) return;
    const auto f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("autosync.log");
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 256 * 1024) f.deleteFile();   // only the latest runs matter
    f.appendText(syncLog_ + "result: " + result + "\n\n");
    syncLog_ = {};
}

// One phase gives five readings 0.4 s of audio apart. They have to agree (a word, a cough or a beat that
// fools one window rarely fools three the same way), else nothing is changed.
Str HubProcessor::steadyLag(double& ms, Str notFound) {
    std::vector<double> found;
    juce::String line;
    const auto readings = syncReads_;
    for (const auto& r : readings) {
        line << (r.ms ? juce::String(*r.ms, 1) : juce::String("--")) << " (" << juce::String(r.score, 2) << ")  ";
        if (r.ms) found.push_back(*r.ms);
    }
    syncLog_ << (autoSync_.phase == SyncPhase::Reference ? "music  ms (score): " : "mic    ms (score): ") << line;
    syncReads_.clear();
    if (found.size() < 3) {
        syncLog_ << "\n";
        return notFound == Str::SyncNoMic && found.size() > 0 ? Str::SyncWeakMic : notFound;
    }
    std::sort(found.begin(), found.end());
    const double median = found[found.size() / 2];
    double sum = 0.0;
    int agree = 0;
    for (double v : found) if (std::abs(v - median) <= 4.0) { sum += v; ++agree; }
    if (agree < 3) {
        syncLog_ << "\n";
        double best = 0.0;
        for (const auto& r : readings) best = std::max(best, r.score);
        return notFound == Str::SyncNoMic && best < 0.5 ? Str::SyncWeakMic : Str::SyncUnsteady;
    }
    ms = sum / agree;
    syncLog_ << "-> " << juce::String(ms, 1) << "\n";
    return Str::None;
}

void HubProcessor::serviceAutoSync() {
    const auto now = juce::Time::getMillisecondCounter();
    auto& st = autoSync_;
    if (st.phase == SyncPhase::Countdown) {
        if (int(now - syncWall_) < 0) return;
        // phase 1: only the music reaches the master; the viewers hear nothing while measuring
        silence_.store(true);
        muteAllBut(st.refSource ? -2 : st.ref, st.refSource ? st.ref : -2);
        syncFrom_ = engine_.historyFrames();
        syncWall_ = now;
        st.phase = SyncPhase::Reference;
        stateChanged.sendChangeMessage();
        return;
    }
    if (st.phase != SyncPhase::Reference && st.phase != SyncPhase::Microphone) return;
    if (now - syncWall_ > 12000) { finishAutoSync(Str::SyncNoAudio); return; }   // the DAW isn't running audio
    // wait until the analysed window is all after the switch (everything mutes within ~50 ms of it)
    const bool music = st.phase == SyncPhase::Reference;
    if (engine_.historyFrames() - syncFrom_ < uint64_t(sampleRate_ * (music ? 1.2 : 2.0)) || now - syncWall_ < 250) return;
    const uint64_t frames = engine_.historyFrames();
    if (!syncReads_.empty() && frames - syncLastRead_ < uint64_t(sampleRate_ * 0.4)) return;
    syncLastRead_ = frames;
    double score = 0.0;
    // phase 2: music that reaches the master another way (an FX return, a channel without HEARASIDE
    // Track) would line up at the music's own lag and outvote the faint mic: take it out first
    const auto lag = engine_.measureLag(st.ref, music ? 0.3 : 0.1, !music, st.refSource, &score, !music,
                                        music ? std::nullopt : std::optional<double>(syncRefMs_));
    syncReads_.push_back({ lag, score });
    if (syncReads_.size() < 5) return;
    if (music) {
        double refLag = 0.0;
        if (const Str e = steadyLag(refLag, Str::SyncNoMusic); e != Str::None) { finishAutoSync(e); return; }
        syncRefMs_ = refLag;
        // phase 2: only the microphone, which hears the music from the headphones
        muteAllBut(st.mic, -2);
        syncFrom_ = engine_.historyFrames();
        syncWall_ = now;
        st.phase = SyncPhase::Microphone;
        stateChanged.sendChangeMessage();
        return;
    }
    double micLag = 0.0;
    if (const Str e = steadyLag(micLag, Str::SyncNoMic); e != Str::None) { finishAutoSync(e); return; }
    st.deltaMs = micLag - syncRefMs_;

    // The voice reaches the viewers deltaMs after the music it was sung to. Shift the music (and
    // everything else) later, or the voice when it is early; the smaller delay ends at 0.
    const auto ts = tracks();
    const auto ss = sources();
    double dv = 0.0, dr = 0.0;
    for (const auto& v : ts) {
        if (v.slot == st.mic) dv = v.delayMs;
        if (!st.refSource && v.slot == st.ref) dr = v.delayMs;
    }
    for (const auto& s : ss) if (st.refSource && s.index == st.ref) dr = s.delayMs;
    const double rel = dv - dr - st.deltaMs;          // wanted voice delay minus music delay
    const double newV = juce::jmax(0.0, rel), newR = juce::jmax(0.0, -rel);
    const double shift = newR - dr;
    auto trackMs = [&](const TrackView& v) { return v.slot == st.mic ? newV : (!st.refSource && v.slot == st.ref) ? newR : double(v.delayMs) + shift; };
    auto sourceMs = [&](const SourceView& s) { return (st.refSource && s.index == st.ref) ? newR : double(s.delayMs) + shift; };
    // nobody is delayed for nothing: whatever reaches the viewers earliest ends at 0 ms (older runs
    // or a removed App Audio can leave every delay high, which only makes the stream later)
    double lowest = std::numeric_limits<double>::max();
    for (const auto& v : ts) if (v.str || v.slot == st.mic) lowest = std::min(lowest, trackMs(v));
    for (const auto& s : ss) if (s.on() || (st.refSource && s.index == st.ref)) lowest = std::min(lowest, sourceMs(s));
    if (lowest == std::numeric_limits<double>::max()) lowest = 0.0;
    auto clampMs = [lowest](double v) { return float(juce::jlimit(0.0, 500.0, v - lowest)); };
    for (const auto& v : ts) {
        const float ms = clampMs(trackMs(v));
        syncLog_ << "  delay " << v.name << ": " << juce::String(v.delayMs, 1) << " -> " << juce::String(ms, 1) << " ms\n";
        if (std::abs(ms - v.delayMs) >= 0.05f) send(v.slot, ssbus::ParamId::StrDelayMs, ms);
    }
    for (const auto& s : ss) {
        const float ms = clampMs(sourceMs(s));
        syncLog_ << "  delay App Audio " << s.name << ": " << juce::String(s.delayMs, 1) << " -> " << juce::String(ms, 1) << " ms\n";
        if (std::abs(ms - s.delayMs) >= 0.05f) sendSource(s.index, ssbus::SourceParam::DelayMs, ms);
    }
    finishAutoSync(Str::None);
}

// ---------------------------------------------------------------------------------------------
// state

void HubProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts_.copyState();
    state.setProperty("bus", busName_, nullptr);
    state.setProperty("lineUp", lineUp_, nullptr);
    state.removeChild(state.getChildWithName("FRIENDS"), nullptr);
    state.appendChild(friendRoom_.toState(), nullptr);
    state.setProperty("listenToken", listenToken_, nullptr);
    state.setProperty("sendToken", sendToken_, nullptr);
    if (shareSecret_.isNotEmpty()) state.setProperty("shareSecret", shareSecret_, nullptr);
    juce::ValueTree stems("STEMS");
    for (int i = 0; i < ssbus::kMaxStems; ++i) stems.setProperty("s" + juce::String(i), stemNames_[size_t(i)], nullptr);
    state.removeChild(state.getChildWithName("STEMS"), nullptr);
    state.appendChild(stems, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void HubProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    const auto stems = state.getChildWithName("STEMS");
    for (int i = 0; i < ssbus::kMaxStems; ++i) stemNames_[size_t(i)] = stems.getProperty("s" + juce::String(i), "").toString();

    // older projects also saved scenes and a hosted mastering chain
    state.removeChild(state.getChildWithName("SCENES"), nullptr);
    state.removeProperty("mastering", nullptr);
    friendRoom_.fromState(state.getChildWithName("FRIENDS"));
    lineUp_ = bool(state.getProperty("lineUp", true));
    state.removeChild(state.getChildWithName("FRIENDS"), nullptr);
    apvts_.replaceState(state);

    const juce::String bus = state.getProperty("bus", defaultBusName());
    const juce::String lt = state.getProperty("listenToken", ""), st = state.getProperty("sendToken", "");
    if (lt.length() >= 8 && st.length() >= 8 && (lt != listenToken_ || st != sendToken_)) {
        listenToken_ = lt;
        sendToken_ = st;
        shareBus_ = nullptr;   // restart with the project's links
    }
    if (const juce::String secret = state.getProperty("shareSecret", ""); secret.length() == 64) shareSecret_ = secret;
    if (bus != busName_) {
        busName_ = bus.isEmpty() ? defaultBusName() : bus;
        lastConnect_ = juce::Time::getMillisecondCounter();
        engine_.connect(busName_.toStdString());
    }
    pushStemNames();
    stateChanged.sendChangeMessage();
}

} // namespace hearaside

#ifndef HEARASIDE_NO_PLUGIN_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hearaside::HubProcessor(); }
#endif
