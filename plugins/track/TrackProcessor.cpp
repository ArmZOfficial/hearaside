#include "TrackProcessor.h"
#include "TrackEditor.h"
#include "FriendDirectory.h"
#include "Strings.h"
#include "ValueText.h"

#include "ssdsp/dsp.h"

namespace hearaside {

using ssengine::TrackPublisher;

namespace {

juce::String dbText(float v, int) {
    if (v <= ssdsp::kMinusInfDb) return "-inf dB";
    return juce::String(v, 1) + " dB";
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout TrackProcessor::createLayout() {
    using namespace juce;
    auto db = [](float lo, float hi) { return NormalisableRange<float>(lo, hi, 0.1f); };
    auto dbAttr = AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(dbText);
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { trackparam::Mon, 1 }, "You Hear", true));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { trackparam::Str, 1 }, "Viewers Hear", true));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { trackparam::StrGain, 1 }, "Viewers Level", db(-60.0f, 12.0f), 0.0f, dbAttr));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { trackparam::StrPan, 1 }, "Viewers Pan",
        NormalisableRange<float>(-100.0f, 100.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) {
            const int i = roundToInt(v);
            return i == 0 ? String("C") : (i < 0 ? "L" + String(-i) : "R" + String(i));
        })));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { trackparam::StrDelay, 1 }, "Viewers Delay",
        NormalisableRange<float>(0.0f, 500.0f, 1.0f), 0.0f, AudioParameterFloatAttributes().withLabel("ms")));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { trackparam::MonTrim, 1 }, "Headphone Level", db(-60.0f, 6.0f), 0.0f, dbAttr));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { trackparam::StrSolo, 1 }, "Viewers Solo", false));
    return { p.begin(), p.end() };
}

TrackProcessor::TrackProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "HEARASIDE_TRACK", createLayout()) {
    mon_   = apvts_.getRawParameterValue(trackparam::Mon);
    str_   = apvts_.getRawParameterValue(trackparam::Str);
    gain_  = apvts_.getRawParameterValue(trackparam::StrGain);
    pan_   = apvts_.getRawParameterValue(trackparam::StrPan);
    delay_ = apvts_.getRawParameterValue(trackparam::StrDelay);
    trim_  = apvts_.getRawParameterValue(trackparam::MonTrim);
    solo_  = apvts_.getRawParameterValue(trackparam::StrSolo);
    connect();
    startTimerHz(30);
}

TrackProcessor::~TrackProcessor() {
    stopTimer();
    disconnectFeeder();
    pub_.disconnect();
}

bool TrackProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void TrackProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    numChannels_ = juce::jlimit(1, 2, getTotalNumOutputChannels());
    for (auto& g : viewersGain_) { g.reset(sampleRate_, 0.020); g.setCurrentAndTargetValue(0.0f); }   // fade in, no clicks
    delayLine_.setSize(2, int(sampleRate_ * 0.5) + juce::jmax(16, samplesPerBlock) + 1);   // Viewers Delay up to 500 ms
    delayLine_.clear();
    delayWrite_ = 0;
    pub_.prepare(uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
    reader_.prepare(sampleRate_, juce::jmax(2048, samplesPerBlock));
    tapSum_ = 0.0f;
    tapPhase_ = 0;
    if (auto* f = feeder_.load(std::memory_order_relaxed))
        f->sampleRate.store(uint32_t(sampleRate_ + 0.5), std::memory_order_relaxed);
}

void TrackProcessor::releaseResources() {
    reader_.reset();
}

void TrackProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process(buffer, false); }

void TrackProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process(buffer, true); }

void TrackProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0) return;   // VST3 parameter flush
    const int numCh = juce::jmin(buffer.getNumChannels(), juce::jmax(1, getTotalNumInputChannels()), 2);
    bypassed_.store(bypassed, std::memory_order_relaxed);
    lastBlock_.store(n, std::memory_order_relaxed);

    if (isFriendInput()) {
        processFriend(buffer, bypassed, n, numCh);
        return;
    }
    processTrack(buffer, bypassed, n, numCh);
}

void TrackProcessor::processFriend(juce::AudioBuffer<float>& buffer, bool bypassed, int n, int numCh) {
    juce::ignoreUnused(numCh);
    auto* f = feeder_.load(std::memory_order_acquire);
    auto* b = feederBus_.load(std::memory_order_acquire);
    const bool offline = isNonRealtime();

    if (f != nullptr) {
        f->heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
        f->blockCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Silence on bypass or offline render
    if (bypassed || offline) {
        if (f != nullptr) {
            f->status.store(offline ? ssbus::kFeederOffline : ssbus::kFeederBypassed, std::memory_order_relaxed);
        }
        reader_.reset();
        buffer.clear();
        for (int c = 0; c < 2; ++c) peak_[c].store(0.0f, std::memory_order_relaxed);
        return;
    }

    const uint32_t fid = friendId_.load(std::memory_order_relaxed);
    if (f == nullptr || b == nullptr || fid == 0) {
        if (f != nullptr) f->status.store(ssbus::kFeederNoFriend, std::memory_order_relaxed);
        reader_.reset();
        buffer.clear();
        for (int c = 0; c < 2; ++c) peak_[c].store(0.0f, std::memory_order_relaxed);
        return;
    }

    int friendSlot = -1;
    for (int s = 0; s < ssbus::kMaxFriends; ++s) {
        if (b->friends[s].id.load(std::memory_order_acquire) == fid) {
            friendSlot = s;
            break;
        }
    }

    if (friendSlot < 0 || b->friends[friendSlot].state.load(std::memory_order_acquire) != ssbus::kFriendLive) {
        f->status.store(ssbus::kFeederFlowing, std::memory_order_relaxed);
        reader_.reset();
        buffer.clear();
        for (int c = 0; c < 2; ++c) peak_[c].store(0.0f, std::memory_order_relaxed);
        return;
    }

    auto& fh = b->friends[friendSlot];
    const uint32_t frRate = fh.sampleRate.load(std::memory_order_relaxed);

    float* out[2];
    out[0] = buffer.getWritePointer(0);
    if (buffer.getNumChannels() > 1) {
        out[1] = buffer.getWritePointer(1);
    } else {
        if (int(monoScratch_.size()) < n) monoScratch_.resize(size_t(n));
        out[1] = monoScratch_.data();
    }

    reader_.pull(b->friendAudio[friendSlot], fh.writePos, frRate, out, n);
    f->status.store(ssbus::kFeederFlowing, std::memory_order_relaxed);

    if (buffer.getNumChannels() == 1) {
        const float* r = monoScratch_.data();
        float* l = buffer.getWritePointer(0);
        for (int i = 0; i < n; ++i) l[i] = (l[i] + r[i]) * 0.5f;
    }
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    const float decay = std::exp(-float(n) / (0.3f * float(sampleRate_)));
    for (int c = 0; c < 2; ++c) {
        const float pk = buffer.getMagnitude(juce::jmin(c, buffer.getNumChannels() - 1), 0, n);
        peak_[c].store(juce::jmax(pk, peak_[c].load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    }

    const int fidx = feederIndex_.load(std::memory_order_relaxed);
    if (fidx >= 0 && fidx < ssbus::kMaxFeeders) {
        auto& tap = b->feederTap[fidx];
        uint64_t tw = f->tapWrite.load(std::memory_order_relaxed);
        const float* l = buffer.getReadPointer(0);
        const float* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : l;
        for (int i = 0; i < n; ++i) {
            tapSum_ += (l[i] + r[i]) * 0.5f;
            if (++tapPhase_ >= ssbus::kTapDecimation) {
                tap.mono[tw % ssbus::kTapFrames] = tapSum_ * (1.0f / float(ssbus::kTapDecimation));
                ++tw;
                tapSum_ = 0.0f;
                tapPhase_ = 0;
            }
        }
        f->tapWrite.store(tw, std::memory_order_release);
    }
}

void TrackProcessor::processTrack(juce::AudioBuffer<float>& buffer, bool bypassed, int n, int numCh) {
    int64_t time = ssengine::kNoTime;
    bool playing = false;
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition()) {
            if (const auto t = pos->getTimeInSamples()) time = *t;
            playing = pos->getIsPlaying();
        }
    }

    const bool mon = mon_->load() > 0.5f;
    pub_.mirror({ mon, str_->load() > 0.5f, solo_->load() > 0.5f, gain_->load(), pan_->load() * 0.01f,
                  delay_->load(), trim_->load(), stem_.load(std::memory_order_relaxed) });
    pub_.process(buffer.getArrayOfReadPointers(), numCh, n, time, playing, isNonRealtime(), bypassed);

    // input meter for the editor (works without a Hub too)
    const float decay = std::exp(-float(n) / (0.3f * float(sampleRate_)));
    for (int c = 0; c < 2; ++c) {
        const float pk = buffer.getMagnitude(juce::jmin(c, numCh - 1), 0, n);
        peak_[c].store(juce::jmax(pk, peak_[c].load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    }

    if (bypassed) return;   // host bypass: audio passes untouched (the editor warns about it)

    // what viewers hear goes to the DAW: Viewers Hear / Solo / Level / Pan / Delay
    const bool solo = solo_->load() > 0.5f;
    const bool on = str_->load() > 0.5f && (solo || !busSolo_.load(std::memory_order_relaxed))
                 && !hubMute_.load(std::memory_order_relaxed);
    const float g = on ? ssdsp::dbToGain(gain_->load()) : 0.0f;
    float gl = 1.0f, gr = 1.0f;
    if (numCh == 2) ssdsp::panGains(pan_->load() * 0.01f, gl, gr);
    viewersGain_[0].setTargetValue(g * gl);
    viewersGain_[1].setTargetValue(g * gr);

    // ponytail: changing the delay jumps (no crossfade); it is set once per song, not automated
    const int len = delayLine_.getNumSamples();
    const int d = len > n ? juce::jlimit(0, len - n, juce::roundToInt(delay_->load() * 0.001 * sampleRate_)) : 0;
    // copy the block in / the delayed block out in at most two pieces (no per-sample modulo)
    auto ringCopy = [len](float* dst, const float* src, int at, int count, bool intoRing) {
        const int first = juce::jmin(count, len - at);
        if (intoRing) { std::memcpy(dst + at, src, size_t(first) * sizeof(float)); std::memcpy(dst, src + first, size_t(count - first) * sizeof(float)); }
        else          { std::memcpy(dst, src + at, size_t(first) * sizeof(float)); std::memcpy(dst + first, src, size_t(count - first) * sizeof(float)); }
    };
    for (int c = 0; c < numCh; ++c) {
        float* x = buffer.getWritePointer(c);
        if (len > n) {   // always record, so turning the delay on later already has history
            float* line = delayLine_.getWritePointer(c);
            ringCopy(line, x, delayWrite_, n, true);
            if (d > 0) ringCopy(x, line, (delayWrite_ - d + len) % len, n, false);
        }
        auto& sg = viewersGain_[c];
        if (sg.isSmoothing()) for (int i = 0; i < n; ++i) x[i] *= sg.getNextValue();
        else if (const float gv = sg.getTargetValue(); gv == 0.0f) juce::FloatVectorOperations::clear(x, n);
        else if (gv != 1.0f) juce::FloatVectorOperations::multiply(x, gv, n);
    }
    if (len > n) delayWrite_ = (delayWrite_ + n) % len;
    for (int c = numCh; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    if (auto* s = pub_.slot(); s && (s->flags.load(std::memory_order_relaxed) & ssbus::kFlagViewersViaHub)) {
        buffer.clear();
    }
}

juce::AudioProcessorEditor* TrackProcessor::createEditor() { return new TrackEditor(*this); }

// ---------------------------------------------------------------------------------------------
// identity, bus connection, remote control

void TrackProcessor::connect() {
    if (isFriendInput()) {
        connectFeeder();
        return;
    }
    lastConnectAttempt_ = juce::Time::getMillisecondCounter();
    const auto oldUuid = uuid_;
    pub_.connect(busName_.toStdString(), uuid_.toStdString(), uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
    uuid_ = juce::String(pub_.uuid());
    if (auto* s = pub_.slot()) {
        cmdCursor_ = s->cmdWrite.load(std::memory_order_acquire);    // ignore commands meant for a previous owner
        renameSeq_ = s->renameSeq.load(std::memory_order_acquire);
        s->chainLatencyBits.store(ssbus::floatBits(chainMs_), std::memory_order_relaxed);
        pub_.prepare(uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
    }
    pushedName_ = {};
    pushIdentity();
    if (oldUuid.isNotEmpty() && oldUuid != uuid_)   // duplicated track got a fresh identity: let the host save it
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void TrackProcessor::connectFeeder() {
    lastConnectAttempt_ = juce::Time::getMillisecondCounter();
    if (feeder_.load(std::memory_order_relaxed) != nullptr) return;
    disconnectFeeder();
    ssbus::SharedMemory::Status st{};
    auto shm = ssbus::SharedMemory::open(busName_.toStdString(), st);
    if (!shm) return;
    const int idx = ssbus::claimFeeder(shm->layout());
    if (idx < 0) {
        retiredFeederShm_.push_back(std::move(shm));
        return;
    }
    feederShm_ = std::move(shm);
    feederBus_.store(&feederShm_->layout(), std::memory_order_release);
    feederIndex_.store(idx, std::memory_order_release);
    auto* f = &feederShm_->layout().feeders[idx];
    const uint32_t fid = friendId_.load(std::memory_order_relaxed);
    f->friendId.store(fid, std::memory_order_relaxed);
    f->sampleRate.store(uint32_t(sampleRate_ + 0.5), std::memory_order_relaxed);
    f->status.store(fid == 0 ? ssbus::kFeederNoFriend : ssbus::kFeederFlowing, std::memory_order_relaxed);
    feeder_.store(f, std::memory_order_release);
    pushFeederIdentity();
    stateChanged.sendChangeMessage();
}

void TrackProcessor::disconnectFeeder() {
    auto* f = feeder_.exchange(nullptr, std::memory_order_acq_rel);
    auto* b = feederBus_.exchange(nullptr, std::memory_order_acq_rel);
    const int idx = feederIndex_.exchange(-1, std::memory_order_acq_rel);
    if (f && b && idx >= 0) {
        ssbus::releaseFeeder(*b, idx);
    }
    if (feederShm_) retiredFeederShm_.push_back(std::move(feederShm_));
    reader_.reset();
}

void TrackProcessor::pushFeederIdentity() {
    auto* f = feeder_.load(std::memory_order_relaxed);
    if (!f) return;
    const auto name = displayName();
    const juce::uint32 colour = hostColour_.isTransparent() ? 0u : hostColour_.getARGB();
    ssbus::setFeederIdentity(*f, name.toStdString(), colour);
}

void TrackProcessor::setRole(Role r) {
    if (role_.load(std::memory_order_relaxed) == r) return;
    role_.store(r, std::memory_order_release);
    if (r == Role::FriendInput) {
        pub_.disconnect();
        connectFeeder();
    } else {
        disconnectFeeder();
        connect();
    }
    reader_.reset();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void TrackProcessor::setFriendId(uint32_t fid) {
    if (friendId_.load(std::memory_order_relaxed) == fid) return;
    friendId_.store(fid, std::memory_order_release);
    if (auto* f = feeder_.load(std::memory_order_relaxed)) {
        f->friendId.store(fid, std::memory_order_relaxed);
        f->status.store(fid == 0 ? ssbus::kFeederNoFriend : ssbus::kFeederFlowing, std::memory_order_relaxed);
    }
    reader_.reset();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

ssbus::BusLayout* TrackProcessor::bus() const noexcept {
    if (isFriendInput()) return feederBus_.load(std::memory_order_acquire);
    return pub_.bus();
}

void TrackProcessor::requestResolveToken(const juce::String& token) {
    auto* b = feederBus_.load(std::memory_order_relaxed);
    const int idx = feederIndex_.load(std::memory_order_relaxed);
    if (b && idx >= 0) {
        pendingRequest_ = ssbus::kReqResolveToken;
        ssbus::postRequest(*b, idx, ssbus::kReqResolveToken, 0, token.toStdString());
    }
}

void TrackProcessor::requestCreateFriend(const juce::String& name) {
    auto* b = feederBus_.load(std::memory_order_relaxed);
    const int idx = feederIndex_.load(std::memory_order_relaxed);
    if (b && idx >= 0) {
        pendingRequest_ = ssbus::kReqCreateFriend;
        ssbus::postRequest(*b, idx, ssbus::kReqCreateFriend, 0, name.toStdString());
    }
}

void TrackProcessor::requestReleaseFriend() {
    auto* b = feederBus_.load(std::memory_order_relaxed);
    const int idx = feederIndex_.load(std::memory_order_relaxed);
    const uint32_t fid = friendId_.load(std::memory_order_relaxed);
    if (b && idx >= 0) {
        pendingRequest_ = ssbus::kReqRelease;
        ssbus::postRequest(*b, idx, ssbus::kReqRelease, fid, {});
    }
}

void TrackProcessor::requestCopyLink(uint32_t fid) {
    auto* b = feederBus_.load(std::memory_order_relaxed);
    const int idx = feederIndex_.load(std::memory_order_relaxed);
    if (b && idx >= 0) {
        pendingRequest_ = ssbus::kReqCopyLink;
        ssbus::postRequest(*b, idx, ssbus::kReqCopyLink, fid, {});
    }
}

uint32_t TrackProcessor::pairedFriendId() const noexcept {
    if (auto* s = pub_.slot()) return s->fedBy.load(std::memory_order_relaxed);
    return 0;
}

float TrackProcessor::fxLatencyMs() const noexcept {
    if (auto* s = pub_.slot()) return ssbus::bitsFloat(s->fxLatencyBits.load(std::memory_order_relaxed));
    return 0.0f;
}

juce::String TrackProcessor::displayName() const {
    if (nameOverride_.isNotEmpty()) return nameOverride_;
    if (hostName_.isNotEmpty()) return hostName_;
    if (isFriendInput()) {
        const uint32_t fid = friendId_.load(std::memory_order_relaxed);
        if (fid != 0) {
            const auto fn = FriendDirectory::nameOf(fid);
            if (fn.isNotEmpty()) return fn;
        }
        return tr(Str::FriendWord);
    }
    return tr(Str::TrackWord) + " " + juce::String(juce::jmax(0, pub_.slotIndex()) + 1);
}

juce::Colour TrackProcessor::trackColour() const { return hostColour_; }

void TrackProcessor::setDisplayNameOverride(const juce::String& name) {
    const auto trimmed = valuetext::truncateUtf8(name.trim(), ssbus::kNameBytes - 1);   // whole code points, NUL fits
    if (trimmed == nameOverride_) return;
    nameOverride_ = trimmed;
    if (isFriendInput()) pushFeederIdentity();
    else pushIdentity();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

juce::String TrackProcessor::busName() const { return busName_; }

void TrackProcessor::setBusName(const juce::String& name) {
    const auto b = name.trim().isEmpty() ? juce::String("Main") : name.trim();
    if (b == busName_) return;
    busName_ = b;
    if (isFriendInput()) {
        disconnectFeeder();
        connectFeeder();
    } else {
        connect();
    }
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

void TrackProcessor::setStemIndex(int s) {
    s = juce::jlimit(-1, ssbus::kMaxStems - 1, s);
    if (s == stem_.load()) return;
    stem_.store(s);
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void TrackProcessor::updateTrackProperties(const TrackProperties& props) {
    const juce::ScopedLock sl(hostLock_);
    pendingHostName_ = props.name.value_or(juce::String());
    pendingHostColour_ = props.colourARGB ? juce::Colour(*props.colourARGB) : juce::Colours::transparentBlack;
    hostDirty_ = true;
}

void TrackProcessor::pushIdentity() {
    const auto name = displayName();
    const juce::uint32 colour = hostColour_.isTransparent() ? 0u : hostColour_.getARGB();
    if (name == pushedName_ && colour == pushedColour_) return;
    pushedName_ = name;
    pushedColour_ = colour;
    pub_.setIdentity(name.toStdString(), colour);
}

void TrackProcessor::setParamPlain(const char* id, float plain) {
    if (auto* p = apvts_.getParameter(id)) {
        const float norm = p->convertTo0to1(plain);
        if (std::abs(norm - p->getValue()) < 1.0e-6f) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(norm);   // host sees it: undo, automation write, project save
        p->endChangeGesture();
    }
}

void TrackProcessor::applyCommand(ssbus::ParamId id, float v) {
    using ssbus::ParamId;
    switch (id) {
        case ParamId::Mon:        setParamPlain(trackparam::Mon, v > 0.5f ? 1.0f : 0.0f); break;
        case ParamId::Str:        setParamPlain(trackparam::Str, v > 0.5f ? 1.0f : 0.0f); break;
        case ParamId::StrGainDb:  setParamPlain(trackparam::StrGain, juce::jlimit(-60.0f, 12.0f, v)); break;
        case ParamId::StrPan:     setParamPlain(trackparam::StrPan, juce::jlimit(-100.0f, 100.0f, v * 100.0f)); break;
        case ParamId::StrDelayMs: setParamPlain(trackparam::StrDelay, juce::jlimit(0.0f, 500.0f, v)); break;
        case ParamId::MonTrimDb:  setParamPlain(trackparam::MonTrim, juce::jlimit(-60.0f, 6.0f, v)); break;
        case ParamId::StrSolo:    setParamPlain(trackparam::StrSolo, v > 0.5f ? 1.0f : 0.0f); break;
        case ParamId::StemIndex:  setStemIndex(juce::roundToInt(v)); break;
    }
}

void TrackProcessor::timerCallback() {
    if (isFriendInput()) {
        auto* b = feederBus_.load(std::memory_order_acquire);
        FriendDirectory::update(b);

        if (feeder_.load(std::memory_order_relaxed) == nullptr
            && juce::Time::getMillisecondCounter() - lastConnectAttempt_ > 2000)
            connectFeeder();

        {
            juce::String name;
            juce::Colour colour;
            bool dirty = false;
            {
                const juce::ScopedLock sl(hostLock_);
                if (hostDirty_) { name = pendingHostName_; colour = pendingHostColour_; hostDirty_ = false; dirty = true; }
            }
            if (dirty && (name != hostName_ || colour != hostColour_)) {
                hostName_ = name;
                hostColour_ = colour;
                stateChanged.sendChangeMessage();
            }
        }
        pushFeederIdentity();

        if (auto* f = feeder_.load(std::memory_order_relaxed)) {
            uint32_t reply = 0;
            if (ssbus::pollReply(*f, replySeq_, reply)) {
                lastReply_.store(reply, std::memory_order_relaxed);
                if ((pendingRequest_ == ssbus::kReqResolveToken || pendingRequest_ == ssbus::kReqCreateFriend)
                    && reply > 0 && reply < 0xFFFF0000u) {
                    setFriendId(reply);
                    pendingRequest_ = 0;
                }
                stateChanged.sendChangeMessage();
            }
            const uint32_t cmdSeq = f->hubCommandSeq.load(std::memory_order_acquire);
            if (cmdSeq != feederCmdSeq_) {
                feederCmdSeq_ = cmdSeq;
                const uint32_t cmd = f->hubCommand.load(std::memory_order_relaxed);
                if (cmd == 1) { // Bring back to the Hub
                    setRole(Role::Track);
                    return;
                }
            }
        }
        return;
    }

    FriendDirectory::update(pub_.bus());
    busSolo_.store(pub_.soloActive(), std::memory_order_relaxed);   // 64-slot scan 30x a second, not every block
    if (pub_.status() != TrackPublisher::Status::Connected
        && juce::Time::getMillisecondCounter() - lastConnectAttempt_ > 2000)
        connect();

    {
        juce::String name;
        juce::Colour colour;
        bool dirty = false;
        {
            const juce::ScopedLock sl(hostLock_);
            if (hostDirty_) { name = pendingHostName_; colour = pendingHostColour_; hostDirty_ = false; dirty = true; }
        }
        if (dirty && (name != hostName_ || colour != hostColour_)) {
            hostName_ = name;
            hostColour_ = colour;
            stateChanged.sendChangeMessage();
        }
    }
    pushIdentity();

    if (auto* s = pub_.slot()) {
        ssbus::pollCommands(*s, cmdCursor_, [this](ssbus::ParamId id, float v) { applyCommand(id, v); });
        std::string renamed;
        if (ssbus::pollRename(*s, renameSeq_, renamed)) setDisplayNameOverride(juce::String::fromUTF8(renamed.c_str()));
    }

    const bool hub = pub_.hubPresent();
    hubMute_.store(hub && pub_.slot() != nullptr && pub_.slot()->hubMute.load(std::memory_order_relaxed) != 0,
                   std::memory_order_relaxed);
    if (pub_.status() != lastStatus_ || hub != lastHub_) {
        lastStatus_ = pub_.status();
        lastHub_ = hub;
        stateChanged.sendChangeMessage();
    }
}

// ---------------------------------------------------------------------------------------------
// state

void TrackProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts_.copyState();
    state.setProperty("uuid", uuid_, nullptr);
    state.setProperty("name", nameOverride_, nullptr);
    state.setProperty("bus", busName_, nullptr);
    state.setProperty("stem", stem_.load(), nullptr);
    if (auto* s = pub_.slot()) chainMs_ = ssbus::bitsFloat(s->chainLatencyBits.load(std::memory_order_relaxed));
    state.setProperty("chainMs", chainMs_, nullptr);
    state.setProperty("role", isFriendInput() ? "friend" : "track", nullptr);
    state.setProperty("friendId", juce::int64(friendId_.load(std::memory_order_relaxed)), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void TrackProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    const juce::String uuid = state.getProperty("uuid", uuid_);
    const juce::String bus = state.getProperty("bus", defaultBusName());
    nameOverride_ = state.getProperty("name", "").toString();
    stem_.store(juce::jlimit(-1, ssbus::kMaxStems - 1, int(state.getProperty("stem", -1))));
    chainMs_ = float(state.getProperty("chainMs", 0.0));
    if (auto* s = pub_.slot()) s->chainLatencyBits.store(ssbus::floatBits(chainMs_), std::memory_order_relaxed);

    const juce::String roleStr = state.getProperty("role", "track").toString();
    const uint32_t fid = uint32_t(juce::int64(state.getProperty("friendId", 0)));
    const Role r = (roleStr == "friend") ? Role::FriendInput : Role::Track;
    friendId_.store(fid, std::memory_order_relaxed);

    apvts_.replaceState(state);
    if (uuid != uuid_ || bus != busName_) {
        uuid_ = uuid;
        busName_ = bus.isEmpty() ? defaultBusName() : bus;
        setRole(r);
    } else {
        setRole(r);
        pushedName_ = {};
        pushIdentity();
        stateChanged.sendChangeMessage();
    }
}

} // namespace hearaside

#ifndef HEARASIDE_NO_PLUGIN_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hearaside::TrackProcessor(); }
#endif
