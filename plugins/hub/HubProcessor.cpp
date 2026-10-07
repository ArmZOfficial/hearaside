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

constexpr juce::uint32 kSceneGraceMs = 1500;   // tracks apply recalled values within ~2 timer ticks

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
    p.push_back(std::make_unique<AudioParameterInt>(ParameterID { hubparam::Scene, 1 }, "Scene", 0, kNumScenes, 0,
        AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return v == 0 ? String("-") : String(v); })));
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
    apvts_.addParameterListener(hubparam::Scene, this);
    headphoneGain_.reset(48000.0, 0.020);
    headphoneGain_.setCurrentAndTargetValue(1.0f);

    lastConnect_ = juce::Time::getMillisecondCounter();
    engine_.connect(busName_.toStdString());
    engine_.prepare(sampleRate_, maxBlock_);
    engine_.setStreamInsert(&mastering_);
    pushStemNames();
    startTimerHz(20);
}

HubProcessor::~HubProcessor() {
    stopTimer();
    apvts_.removeParameterListener(hubparam::Scene, this);
    engine_.setStreamInsert(nullptr);
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
    mastering_.prepare(sampleRate_, maxBlock_);
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
    hp.preview = preview_->load() > 0.5f;
    hp.panic = panic_->load() > 0.5f;
    hp.syncSafety = juce::roundToInt(sync_->load());
    hp.bypassed = bypassed;
    const bool offline = isNonRealtime();
    mastering_.setPlayHead(getPlayHead());
    engine_.process(buffer.getArrayOfWritePointers(), numCh, n, hp, time, playing, offline);
    if (bypassed) return;

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

// ---------------------------------------------------------------------------------------------
// scenes

juce::String HubProcessor::sceneName(int i) const {
    const auto& s = scene(i);
    if (s.name.isNotEmpty()) return s.name;
    switch (i) {
        case 0: return tr(Str::SceneSinging);
        case 1: return tr(Str::SceneTalking);
        case 2: return tr(Str::SceneBrb);
        default: return tr(Str::SceneCustom) + " " + juce::String(i + 1);
    }
}

void HubProcessor::recallScene(int i) {
    // Going through the parameter lets the host record / automate scene changes.
    setParam(hubparam::Scene, float(i + 1));
    pendingScene_.store(i + 1);   // also when the parameter already had this value
}

void HubProcessor::parameterChanged(const juce::String& id, float value) {
    if (id == hubparam::Scene && juce::roundToInt(value) > 0) pendingScene_.store(juce::roundToInt(value));
}

void HubProcessor::applyScene(int i) {
    const auto& sc = scene(i);
    if (!sc.saved()) { activeScene_ = i; stateChanged.sendChangeMessage(); return; }
    for (const auto& t : tracks()) {
        const auto it = sc.tracks.find(t.uuid);
        if (it == sc.tracks.end()) continue;   // tracks created after the scene are left alone
        const auto& v = it->second;
        using ssbus::ParamId;
        send(t.slot, ParamId::Mon, v.mon ? 1.0f : 0.0f);
        send(t.slot, ParamId::Str, v.str ? 1.0f : 0.0f);
        send(t.slot, ParamId::StrGainDb, v.gainDb);
        send(t.slot, ParamId::StrPan, v.pan);
        send(t.slot, ParamId::StrSolo, v.solo ? 1.0f : 0.0f);
        send(t.slot, ParamId::MonTrimDb, v.trimDb);
        send(t.slot, ParamId::StrDelayMs, v.delayMs);
    }
    activeScene_ = i;
    sceneRecallMs_ = juce::Time::getMillisecondCounter();
    stateChanged.sendChangeMessage();
}

bool HubProcessor::sceneMatches(int i) const {
    const auto& sc = scene(i);
    for (const auto& t : tracks()) {
        const auto it = sc.tracks.find(t.uuid);
        if (it == sc.tracks.end()) continue;
        const auto& v = it->second;
        if (v.mon != t.mon || v.str != t.str || v.solo != t.solo || std::abs(v.gainDb - t.gainDb) > 0.06f
            || std::abs(v.trimDb - t.trimDb) > 0.06f || std::abs(v.pan - t.pan) > 0.006f || std::abs(v.delayMs - t.delayMs) > 0.6f)
            return false;
    }
    return true;
}

void HubProcessor::saveScene(int i) {
    auto& sc = scenes_[size_t(juce::jlimit(0, kNumScenes - 1, i))];
    sc.tracks.clear();
    for (const auto& t : tracks())
        sc.tracks[t.uuid] = SceneTrack { t.mon, t.str, t.solo, t.gainDb, t.pan, t.trimDb, t.delayMs };
    if (sc.name.isEmpty() && i >= 3) sc.name = sceneName(i);
    activeScene_ = i;
    sceneRecallMs_ = juce::Time::getMillisecondCounter();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void HubProcessor::renameScene(int i, const juce::String& name) {
    scenes_[size_t(juce::jlimit(0, kNumScenes - 1, i))].name = name.trim();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void HubProcessor::clearScene(int i) {
    auto& sc = scenes_[size_t(juce::jlimit(0, kNumScenes - 1, i))];
    sc.tracks.clear();
    if (i >= 3) sc.name = {};
    if (activeScene_ == i) activeScene_ = -1;
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
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
        mastering_.refreshLatency();   // hosted plug-ins may change their latency at any time
        const auto before = engine_.role();
        engine_.maintain();
        if (engine_.role() != before) { if (engine_.role() == HubEngine::Role::Owner) pushStemNames(); stateChanged.sendChangeMessage(); }
        if (activeScene_ >= 0 && scene(activeScene_).saved() && now - sceneRecallMs_ > kSceneGraceMs && !sceneMatches(activeScene_)) {
            activeScene_ = -1;   // something was changed by hand: no scene is selected any more
            stateChanged.sendChangeMessage();
        }
    }
    serviceRemote();
    const int pending = pendingScene_.exchange(0);
    // parameter notifications caused by loading a project may arrive late: never treat them as a recall
    if (pending > 0 && now - stateLoadMs_ > 1000) applyScene(pending - 1);
}

// ---------------------------------------------------------------------------------------------
// remote control (OBS dock, OBS hotkeys)

void HubProcessor::serviceRemote() {
    auto* bus = engine_.bus();
    if (bus == nullptr || engine_.role() != HubEngine::Role::Owner) { remoteBus_ = nullptr; return; }
    if (bus != remoteBus_) {   // new bus or just became the owner: ignore commands queued before
        remoteBus_ = bus;
        remoteCursor_ = bus->header.remoteReserve.load(std::memory_order_acquire);
    }
    ssbus::pollRemote(*bus, remoteCursor_, [this](int target, uint32_t pid, float v) { applyRemote(target, pid, v); });

    ssbus::HubStateView st;
    st.masterDb = master_->load();
    st.headphonesDb = headphones_->load();
    st.ceilingDb = ceiling_->load();
    st.activeScene = activeScene_;
    st.syncSafety = uint32_t(juce::roundToInt(sync_->load()));
    for (int i = 0; i < kNumScenes && i < ssbus::kMaxScenes; ++i) {
        if (i < 3 || scene(i).saved() || scene(i).name.isNotEmpty()) st.sceneMask |= 1u << i;
        st.sceneNames[i] = sceneName(i).toStdString();
    }
    ssbus::publishHubState(*bus, st);
}

void HubProcessor::applyRemote(int target, uint32_t pid, float v) {
    using ssbus::RemoteParam;
    if (target >= 0) {   // forwarded to a Track: the Track applies it through its host
        if (pid >= uint32_t(ssbus::ParamId::Mon) && pid <= uint32_t(ssbus::ParamId::StemIndex))
            send(target, static_cast<ssbus::ParamId>(pid), v);
        return;
    }
    switch (static_cast<RemoteParam>(pid)) {
        case RemoteParam::Panic:        setParam(hubparam::Panic, v > 0.5f ? 1.0f : 0.0f); break;
        case RemoteParam::Preview:      setParam(hubparam::Preview, v > 0.5f ? 1.0f : 0.0f); break;
        case RemoteParam::LimiterOn:    setParam(hubparam::LimiterOn, v > 0.5f ? 1.0f : 0.0f); break;
        case RemoteParam::MasterDb:     setParam(hubparam::Master, juce::jlimit(-60.0f, 12.0f, v)); break;
        case RemoteParam::HeadphonesDb: setParam(hubparam::Headphones, juce::jlimit(-60.0f, 6.0f, v)); break;
        case RemoteParam::SyncSafety:   setParam(hubparam::SyncSafety, float(juce::jlimit(0, 2, juce::roundToInt(v)))); break;
        case RemoteParam::RecallScene: {
            const int i = juce::roundToInt(v);
            if (i >= 0 && i < kNumScenes) recallScene(i);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// state

void HubProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts_.copyState();
    state.setProperty("bus", busName_, nullptr);
    juce::ValueTree scenes("SCENES");
    for (int i = 0; i < kNumScenes; ++i) {
        const auto& sc = scenes_[size_t(i)];
        juce::ValueTree s("SCENE");
        s.setProperty("index", i, nullptr);
        s.setProperty("name", sc.name, nullptr);
        for (const auto& [uuid, v] : sc.tracks) {
            juce::ValueTree t("T");
            t.setProperty("uuid", uuid, nullptr);
            t.setProperty("mon", v.mon, nullptr);
            t.setProperty("str", v.str, nullptr);
            t.setProperty("solo", v.solo, nullptr);
            t.setProperty("gain", v.gainDb, nullptr);
            t.setProperty("pan", v.pan, nullptr);
            t.setProperty("trim", v.trimDb, nullptr);
            t.setProperty("delay", v.delayMs, nullptr);
            s.appendChild(t, nullptr);
        }
        scenes.appendChild(s, nullptr);
    }
    state.removeChild(state.getChildWithName("SCENES"), nullptr);
    state.appendChild(scenes, nullptr);
    juce::ValueTree stems("STEMS");
    for (int i = 0; i < ssbus::kMaxStems; ++i) stems.setProperty("s" + juce::String(i), stemNames_[size_t(i)], nullptr);
    state.removeChild(state.getChildWithName("STEMS"), nullptr);
    state.appendChild(stems, nullptr);
    if (auto m = mastering_.toXml()) state.setProperty("mastering", m->toString(juce::XmlElement::TextFormat().singleLine()), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void HubProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    for (auto& sc : scenes_) { sc.name = {}; sc.tracks.clear(); }
    for (const auto& s : state.getChildWithName("SCENES")) {
        const int i = s.getProperty("index", -1);
        if (i < 0 || i >= kNumScenes) continue;
        auto& sc = scenes_[size_t(i)];
        sc.name = s.getProperty("name", "").toString();
        for (const auto& t : s) {
            SceneTrack v;
            v.mon = t.getProperty("mon", true);
            v.str = t.getProperty("str", true);
            v.solo = t.getProperty("solo", false);
            v.gainDb = float(t.getProperty("gain", 0.0));
            v.pan = float(t.getProperty("pan", 0.0));
            v.trimDb = float(t.getProperty("trim", 0.0));
            v.delayMs = float(t.getProperty("delay", 0.0));
            sc.tracks[t.getProperty("uuid").toString()] = v;
        }
    }
    const auto stems = state.getChildWithName("STEMS");
    for (int i = 0; i < ssbus::kMaxStems; ++i) stemNames_[size_t(i)] = stems.getProperty("s" + juce::String(i), "").toString();

    const juce::String masteringXml = state.getProperty("mastering", "").toString();
    state.removeProperty("mastering", nullptr);
    apvts_.replaceState(state);
    if (auto m = juce::parseXML(masteringXml)) {
        // hosted plug-ins must be created on the message thread
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) mastering_.fromXml(*m);
        else {
            std::shared_ptr<juce::XmlElement> shared(m.release());
            juce::MessageManager::callAsync([this, shared] { mastering_.fromXml(*shared); });
        }
    }
    pendingScene_.store(0);   // loading a project restores each Track's own state; never re-apply a scene
    const int sceneParam = juce::roundToInt(apvts_.getRawParameterValue(hubparam::Scene)->load());
    activeScene_ = sceneParam > 0 ? sceneParam - 1 : -1;
    sceneRecallMs_ = stateLoadMs_ = juce::Time::getMillisecondCounter();

    const juce::String bus = state.getProperty("bus", defaultBusName());
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
