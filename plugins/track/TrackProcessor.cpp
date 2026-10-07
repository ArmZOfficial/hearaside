#include "TrackProcessor.h"
#include "TrackEditor.h"
#include "Strings.h"

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
    monitorGain_.reset(48000.0, 0.010);
    monitorGain_.setCurrentAndTargetValue(1.0f);
    connect();
    startTimerHz(30);
}

TrackProcessor::~TrackProcessor() {
    stopTimer();
    pub_.disconnect();
}

bool TrackProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void TrackProcessor::prepareToPlay(double sampleRate, int) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    numChannels_ = juce::jlimit(1, 2, getTotalNumOutputChannels());
    const float target = mon_->load() > 0.5f ? ssdsp::dbToGain(trim_->load()) : 0.0f;
    monitorGain_.reset(sampleRate_, 0.010);   // ~10 ms ramp, no clicks
    monitorGain_.setCurrentAndTargetValue(target);
    pub_.prepare(uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
}

void TrackProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process(buffer, false); }

void TrackProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process(buffer, true); }

void TrackProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0) return;   // VST3 parameter flush
    const int numCh = juce::jmin(buffer.getNumChannels(), juce::jmax(1, getTotalNumInputChannels()), 2);
    bypassed_.store(bypassed, std::memory_order_relaxed);

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
    monitorGain_.setTargetValue(mon ? ssdsp::dbToGain(trim_->load()) : 0.0f);
    if (monitorGain_.isSmoothing()) {
        for (int i = 0; i < n; ++i) {
            const float g = monitorGain_.getNextValue();
            for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.getWritePointer(c)[i] *= g;
        }
    } else {
        const float g = monitorGain_.getTargetValue();
        if (g == 0.0f) buffer.clear();
        else if (g != 1.0f) buffer.applyGain(g);
    }
}

juce::AudioProcessorEditor* TrackProcessor::createEditor() { return new TrackEditor(*this); }

// ---------------------------------------------------------------------------------------------
// identity, bus connection, remote control

void TrackProcessor::connect() {
    lastConnectAttempt_ = juce::Time::getMillisecondCounter();
    const auto oldUuid = uuid_;
    pub_.connect(busName_.toStdString(), uuid_.toStdString(), uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
    uuid_ = juce::String(pub_.uuid());
    if (auto* s = pub_.slot()) {
        cmdCursor_ = s->cmdWrite.load(std::memory_order_acquire);    // ignore commands meant for a previous owner
        renameSeq_ = s->renameSeq.load(std::memory_order_acquire);
        pub_.prepare(uint32_t(sampleRate_ + 0.5), uint32_t(numChannels_));
    }
    pushedName_ = {};
    pushIdentity();
    if (oldUuid.isNotEmpty() && oldUuid != uuid_)   // duplicated track got a fresh identity: let the host save it
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

juce::String TrackProcessor::displayName() const {
    if (nameOverride_.isNotEmpty()) return nameOverride_;
    if (hostName_.isNotEmpty()) return hostName_;
    return tr(Str::TrackWord) + " " + juce::String(juce::jmax(0, pub_.slotIndex()) + 1);
}

juce::Colour TrackProcessor::trackColour() const { return hostColour_; }

void TrackProcessor::setDisplayNameOverride(const juce::String& name) {
    const auto trimmed = name.trim();
    if (trimmed == nameOverride_) return;
    nameOverride_ = trimmed;
    pushIdentity();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

juce::String TrackProcessor::busName() const { return busName_; }

void TrackProcessor::setBusName(const juce::String& name) {
    const auto b = name.trim().isEmpty() ? juce::String("Main") : name.trim();
    if (b == busName_) return;
    busName_ = b;
    connect();
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
    apvts_.replaceState(state);
    if (uuid != uuid_ || bus != busName_) {
        uuid_ = uuid;
        busName_ = bus.isEmpty() ? defaultBusName() : bus;
        connect();
    } else {
        pushedName_ = {};
        pushIdentity();
        stateChanged.sendChangeMessage();
    }
}

} // namespace hearaside

#ifndef HEARASIDE_NO_PLUGIN_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hearaside::TrackProcessor(); }
#endif
