#include "AppAudioProcessor.h"
#include "Host.h"
#include "Strings.h"
#include "ui/Components.h"
#include "ui/EditorShell.h"

#include "ssdsp/dsp.h"

namespace hearaside {

using ssbus::SourceParam;

namespace {

juce::String dbText(float v, int) {
    if (v <= ssdsp::kMinusInfDb) return "-inf dB";
    return juce::String(v, 1) + " dB";
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout AppAudioProcessor::createLayout() {
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { appparam::On, 1 }, "Capture On", true));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { appparam::Level, 1 }, "Program Level",
        NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(dbText)));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { appparam::Delay, 1 }, "Sync Delay",
        NormalisableRange<float>(0.0f, 500.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel("ms")));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { appparam::Only, 1 }, "Program Only", false));
    auto dbAttr = AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(dbText);
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { appparam::Mon, 1 }, "You Hear", true));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { appparam::Str, 1 }, "Viewers Hear", true));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { appparam::StrGain, 1 }, "Viewers Level",
        NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f, dbAttr));
    p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { appparam::MonTrim, 1 }, "Headphone Level",
        NormalisableRange<float>(-60.0f, 6.0f, 0.1f), 0.0f, dbAttr));
    return { p.begin(), p.end() };
}

AppAudioProcessor::AppAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "HEARASIDE_APP", createLayout()) {
    on_ = apvts_.getRawParameterValue(appparam::On);
    level_ = apvts_.getRawParameterValue(appparam::Level);
    delay_ = apvts_.getRawParameterValue(appparam::Delay);
    only_ = apvts_.getRawParameterValue(appparam::Only);
    mon_ = apvts_.getRawParameterValue(appparam::Mon);
    str_ = apvts_.getRawParameterValue(appparam::Str);
    strGain_ = apvts_.getRawParameterValue(appparam::StrGain);
    monTrim_ = apvts_.getRawParameterValue(appparam::MonTrim);
    connectBus();
    connectTrack();
    startTimerHz(20);
}

AppAudioProcessor::~AppAudioProcessor() {
    stopTimer();
    pub_.disconnect();
    rec_.close();
    capture_.stop();
    link_.stop();
    if (src_.exchange(nullptr) != nullptr && bus_ != nullptr) ssbus::releaseSource(*bus_, srcIndex_);
}

bool AppAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
}

void AppAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    preparing_.store(true);
    const double sr = sampleRate > 0 ? sampleRate : 48000.0;
    const bool rateChanged = std::abs(sr - sampleRate_) > 0.5 || rs_ == nullptr;
    sampleRate_ = sr;
    maxBlock_ = juce::jlimit(16, 8192, samplesPerBlock);
    // The program's audio is captured at the DAW rate; the resampler only absorbs the clock drift
    // between the Windows audio engine and the DAW's interface.
    const uint32_t r = uint32_t(sampleRate_ + 0.5);
    rs_ = std::make_unique<ssdsp::VarResampler>(r, r, 4);
    rs48_ = std::make_unique<ssdsp::VarResampler>(48000, r, 4);
    rs441_ = std::make_unique<ssdsp::VarResampler>(44100, r, 4);
    feedKey_ = 0;   // pull() sets the buffer up for the input
    // safe start: one DAW block + one capture packet + 2 ms; pull() trims it to the measured need
    targetFrames_ = double(maxBlock_) + double(capture_.packetFrames()) * sampleRate_ / 48000.0 + sampleRate_ * 0.002;
    floorFrames_ = double(maxBlock_) + 32.0;
    holdSec_ = 0.0;
    for (auto& b : in_) b.assign(size_t(maxBlock_) * 2 + 256, 0.0f);
    for (auto& b : appBuf_) b.assign(size_t(maxBlock_), 0.0f);
    delayLine_.setSize(2, int(sampleRate_ * 0.5) + maxBlock_ + 1);
    delayLine_.clear();
    delayWrite_ = 0;
    gain_.reset(sampleRate_, 0.020);
    gain_.setCurrentAndTargetValue(0.0f);
    fade_.reset(sampleRate_, 0.005);
    fade_.setCurrentAndTargetValue(0.0f);
    viewers_.reset(sampleRate_, 0.020);
    viewers_.setCurrentAndTargetValue(0.0f);
    pub_.prepare(r, 2);
    primed_ = false;
    if (rateChanged) rec_.prepare(sampleRate_);   // a take keeps one sample rate
    preparing_.store(false);
    if (rateChanged && capture_.state() != AppCapture::State::Idle) restartCapture();
}

void AppAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    const int n = buffer.getNumSamples();
    if (n == 0 || preparing_.load()) return;
    const int numCh = juce::jmin(buffer.getNumChannels(), 2);

    int64_t time = 0;
    bool hasTime = false, recording = false;
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition()) {
            if (const auto t = pos->getTimeInSamples()) { time = *t; hasTime = pos->getIsPlaying(); }
            recording = pos->getIsRecording();
        }
    }
    if (auto* s = src_.load(std::memory_order_acquire)) s->heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
    pub_.mirror({ mon_->load() > 0.5f, str_->load() > 0.5f, false, strGain_->load(), 0.0f, delay_->load(), monTrim_->load(), -1, true });
    const bool viewersOn = str_->load() > 0.5f && !busSolo_.load(std::memory_order_relaxed) && !hubMute_.load(std::memory_order_relaxed);
    viewers_.setTargetValue(viewersOn ? ssdsp::dbToGain(strGain_->load()) : 0.0f);

    for (int off = 0; off < n; off += maxBlock_) {
        const int m = juce::jmin(maxBlock_, n - off);
        float* io[2] = { buffer.getWritePointer(0, off), numCh > 1 ? buffer.getWritePointer(1, off) : nullptr };
        processChunk(io, juce::jmax(1, numCh), m, time + off, hasTime, recording);
    }
}

void AppAudioProcessor::processChunk(float* const* io, int numCh, int n, int64_t time, bool hasTime, bool hostRecording) {
    float* a[2] = { appBuf_[0].data(), appBuf_[1].data() };
    // exports render faster than real time: a live program can't follow, so they get the track only
    const bool on = on_->load() > 0.5f && !isNonRealtime();
    gain_.setTargetValue(on ? ssdsp::dbToGain(level_->load()) : 0.0f);

    bool have = false;
    if (on || gain_.isSmoothing()) {
        have = pull(a, n);
    } else if (primed_) {   // switched off: start fresh (no stale audio) when switched on again
        primed_ = false;
        feedKey_ = 0;
    }

    float pk = 0.0f;
    if (have) {
        for (int i = 0; i < n; ++i) {
            const float g = gain_.getNextValue() * fade_.getNextValue();
            a[0][i] *= g;
            a[1][i] *= g;
        }
        pk = juce::jmax(juce::FloatVectorOperations::findMaximum(a[0], n), -juce::FloatVectorOperations::findMinimum(a[0], n),
                        juce::FloatVectorOperations::findMaximum(a[1], n), -juce::FloatVectorOperations::findMinimum(a[1], n));
    } else {
        gain_.skip(n);
        juce::FloatVectorOperations::clear(a[0], n);
        juce::FloatVectorOperations::clear(a[1], n);
    }
    peak_.store(juce::jmax(pk, peak_.load(std::memory_order_relaxed) * std::exp(-float(n) / (0.3f * float(sampleRate_)))),
                std::memory_order_relaxed);
    juce::ignoreUnused(numCh);
    finishChunk(io, numCh, n, have, time, hasTime, hostRecording);
}

void AppAudioProcessor::finishChunk(float* const* io, int numCh, int n, bool have, int64_t time, bool hasTime, bool hostRecording) {
    float* a[2] = { appBuf_[0].data(), appBuf_[1].data() };
    // the Hub's auto sync measures this signal (before the sync delay)
    if (auto* s = src_.load(std::memory_order_acquire))
        if (auto* ring = srcRing_.load(std::memory_order_acquire))
            ssbus::ringWrite(*ring, s->writePos, a, 2, uint32_t(n));
    // your headphones (the Hub, "you hear"): the program as it is now, without the viewers' sync delay
    pub_.process(a, 2, n, time, hasTime, isNonRealtime(), false);

    // sync delay (always running, so a new delay already has history)
    const int len = delayLine_.getNumSamples();
    const int d = len > n ? juce::jlimit(0, len - n, juce::roundToInt(delay_->load() * 0.001 * sampleRate_)) : 0;
    if (len > n) {
        for (int c = 0; c < 2; ++c) {
            float* line = delayLine_.getWritePointer(c);
            const int first = juce::jmin(n, len - delayWrite_);
            std::memcpy(line + delayWrite_, a[c], size_t(first) * sizeof(float));
            std::memcpy(line, a[c] + first, size_t(n - first) * sizeof(float));
            if (d > 0) {
                const int from = (delayWrite_ - d + len) % len;
                const int f2 = juce::jmin(n, len - from);
                std::memcpy(a[c], line + from, size_t(f2) * sizeof(float));
                std::memcpy(a[c] + f2, line, size_t(n - f2) * sizeof(float));
            }
        }
        delayWrite_ = (delayWrite_ + n) % len;
    }

    if (only_->load() > 0.5f)   // an input channel of its own: the DAW records the program and nothing else
        for (int c = 0; c < numCh; ++c) juce::FloatVectorOperations::clear(io[c], n);
    // the viewers (the DAW): Viewers Hear x Viewers Level
    const bool viewers = viewers_.isSmoothing() || viewers_.getTargetValue() > 0.0f;
    if (viewers_.isSmoothing()) {
        for (int i = 0; i < n; ++i) { const float g = viewers_.getNextValue(); a[0][i] *= g; a[1][i] *= g; }
    } else if (const float g = viewers_.getTargetValue(); viewers && g != 1.0f) {
        juce::FloatVectorOperations::multiply(a[0], g, n);
        juce::FloatVectorOperations::multiply(a[1], g, n);
    }
    if ((have || d > 0) && viewers) {
        if (numCh == 2) {
            juce::FloatVectorOperations::add(io[0], a[0], n);
            juce::FloatVectorOperations::add(io[1], a[1], n);
        } else {
            for (int i = 0; i < n; ++i) io[0][i] += 0.5f * (a[0][i] + a[1][i]);
        }
    }

    // printing: what this plug-in adds to the track, silence included, so the take stays on the timeline
    rec_.write(a, n, manualRec_.load(std::memory_order_relaxed) || (follow_.load(std::memory_order_relaxed) && hostRecording),
               time, hasTime);
}

AppAudioProcessor::Feed AppAudioProcessor::feed() const noexcept {
    const uint32_t r = uint32_t(sampleRate_ + 0.5);
    switch (Input(mode_.load(std::memory_order_acquire))) {
        case Input::LinkIn:
            if (auto* b = linkBus_.load(std::memory_order_acquire)) {
                const uint32_t lr = b->linkIn.sampleRate.load(std::memory_order_relaxed);
                return { &b->linkInAudio, &b->linkIn.writePos, lr, lr * 0.03 };
            }
            return { nullptr, nullptr, 0, 0 };
        case Input::Link:
            return { &link_.ring(), &link_.writePos(), link_.sampleRate(), link_.sampleRate() * 0.03 };
        case Input::Program:
            break;
    }
    return { &capture_.ring(), &capture_.writePos(), r, double(capture_.packetFrames()) * r / 48000.0 };
}

bool AppAudioProcessor::pull(float* const* out, int n) {
    const double sr = sampleRate_;
    const Feed f = feed();
    const uint32_t r = uint32_t(sr + 0.5);
    ssdsp::VarResampler* rs = f.rate == r ? rs_.get() : f.rate == 48000 ? rs48_.get() : f.rate == 44100 ? rs441_.get() : nullptr;
    if (f.ring == nullptr || rs == nullptr) return false;
    const double fr = double(f.rate);   // frames of the input per second (buffer sizes are in input frames)
    const uint64_t w = f.writePos->load(std::memory_order_acquire);
    const uint64_t key = (uint64_t(mode_.load(std::memory_order_relaxed)) << 32) | f.rate;
    if (key != feedKey_) {   // new input or rate: safe start (network links get extra room for jitter)
        feedKey_ = key;
        const double block = double(maxBlock_) * fr / sr;
        targetFrames_ = block + f.packet + fr * (mode_.load() != 0 ? 0.04 : 0.002);
        floorFrames_ = block + 32.0;
        holdSec_ = 0.0;
        primed_ = false;
        starvedAt_ = lastWrite_ = w;
    }
    // The input is "playing" if a packet came in the last 3 packet periods. (Per block is not
    // enough: with DAW blocks shorter than a packet, many blocks see no new packet.)
    sinceWrite_ = w != lastWrite_ ? 0.0 : sinceWrite_ + double(n) / sr;
    lastWrite_ = w;
    const bool arriving = sinceWrite_ < 3.0 * f.packet / fr + double(n) / sr;
    const double target = targetFrames_;
    double fill = double(int64_t(w - cursor_));
    const double maxFill = double(ssbus::kRingFrames - ssbus::kGuardFrames);
    // (re)start once enough new audio has arrived: after start-up, after the program fell silent,
    // or when the DAW stalled and the buffer grew far beyond the target (keeps the delay low)
    if (!primed_ || fill < 0.0 || fill > juce::jmin(maxFill, target * 2.0 + fr * 0.05)) {
        if (double(w) < double(starvedAt_) + target + n) return false;
        cursor_ = w - uint64_t(target);
        drift_.reset(target);
        rs->reset();
        primed_ = true;
        fill = target;
        fade_.setCurrentAndTargetValue(0.0f);   // no click when the sound comes back
        fade_.setTargetValue(1.0f);
        minMargin_ = 1.0e9;
        windowSec_ = 0.0;
        fillAvg_ = target;
    }
    const double need = double(rs->inputFor(uint32_t(n)));
    if (fill < need) {
        // Packets still arriving but not enough of them: the timing needs more headroom.
        // No packets: the program simply stopped playing.
        if (arriving) {
            targetFrames_ = juce::jmin(targetFrames_ + fr * 0.002, double(maxBlock_) * fr / sr + fr * 0.4);
            floorFrames_ = targetFrames_;   // learned: the timing here needs at least this much
            holdSec_ = 5.0;
        }
        primed_ = false;
        starvedAt_ = w;
        return false;
    }
    // adaptive buffer: every 3 s, give back half of the headroom above 2 ms that was never used
    // (only once the buffer really sits at the target: the drift control lowers it gently)
    const double dt = double(n) / sr;
    fillAvg_ += (fill - fillAvg_) * (1.0 - std::exp(-dt / 0.3));
    minMargin_ = juce::jmin(minMargin_, fill - need);
    windowSec_ += dt;
    holdSec_ -= dt;
    if (windowSec_ >= 3.0) {
        const double spare = minMargin_ - fr * 0.002;
        if (holdSec_ <= 0.0 && spare > fr * 0.0002 && std::abs(fillAvg_ - targetFrames_) < fr * 0.0005) {
            targetFrames_ = juce::jmax(juce::jmax(floorFrames_, double(n) * fr / sr + 32.0), targetFrames_ - juce::jmin(spare * 0.5, fr * 0.001));
            drift_.setTarget(targetFrames_);
        }
        windowSec_ = 0.0;
        minMargin_ = 1.0e9;
    }
    latencyMs_.store(fillAvg_ * 1000.0 / fr, std::memory_order_relaxed);

    const double c = drift_.update(fill, double(n) / sr);
    rs->setCorrectionPpm(int(std::lround(c * 1.0e6)));
    float* in[2] = { in_[0].data(), in_[1].data() };
    uint32_t made = 0;
    for (int pass = 0; pass < 3 && made < uint32_t(n); ++pass) {
        const uint32_t want = std::min<uint32_t>(rs->inputFor(uint32_t(n) - made), uint32_t(in_[0].size()));
        uint32_t avail = 0;
        if (ssbus::ringReadAt(*f.ring, *f.writePos, cursor_, in, want, &avail) == ssbus::ReadResult::Overrun) {
            primed_ = false;
            starvedAt_ = w;
            return false;
        }
        if (avail == 0) break;
        uint32_t used = avail, got = uint32_t(n) - made;
        float* o[2] = { out[0] + made, out[1] + made };
        rs->process(in, used, o, got);
        cursor_ += used;
        made += got;
        if (used == 0 && got == 0) break;
    }
    for (int c2 = 0; c2 < 2; ++c2)
        if (made < uint32_t(n)) juce::FloatVectorOperations::clear(out[c2] + made, n - int(made));
    return true;
}

// ---------------------------------------------------------------------------------------------
// message thread

juce::String AppAudioProcessor::appLabel(const juce::String& exe) {
    if (exe.isEmpty()) return {};
    if (exe == kSystemAudio) return tr(Str::AppSystem);
    if (exe == kLinkIn) return tr(Str::AppLinkIn);
    if (LinkReceiver::isLink(exe.toStdString())) return tr(Str::AppLinkFrom) + " " + juce::URL(exe).getDomain();
    return exe.endsWithIgnoreCase(".exe") ? exe.dropLastCharacters(4) : exe;
}

juce::String AppAudioProcessor::displayName() const {
    {
        const juce::ScopedLock sl(hostLock_);
        if (hostName_.isNotEmpty()) return hostName_;
    }
    return app_.isNotEmpty() ? appLabel(app_) : juce::String("App Audio");
}

void AppAudioProcessor::updateTrackProperties(const TrackProperties& props) {
    const juce::ScopedLock sl(hostLock_);
    hostName_ = props.name.value_or(juce::String());
    hostColour_ = props.colourARGB.value_or(0u);
}

void AppAudioProcessor::setApp(const juce::String& exe) {
    if (exe == app_) return;
    app_ = exe;
    restartCapture();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void AppAudioProcessor::restartCapture() {
    const bool linkIn = app_ == kLinkIn, link = LinkReceiver::isLink(app_.toStdString());
    mode_.store(int(linkIn ? Input::LinkIn : link ? Input::Link : Input::Program), std::memory_order_release);
    if (app_.isEmpty() || !isOn()) { capture_.stop(); link_.stop(); return; }
    if (linkIn) { capture_.stop(); link_.stop(); return; }   // the Hub's share server fills the bus
    if (link) { capture_.stop(); link_.start(app_.toStdString()); return; }
    link_.stop();
    if (app_ == kSystemAudio) capture_.start(ssbus::currentPid(), uint32_t(sampleRate_ + 0.5), true);
    else capture_.start(AppCapture::findRootProcess(app_.toStdString()), uint32_t(sampleRate_ + 0.5));
}

AppCapture::State AppAudioProcessor::captureState() const noexcept {
    switch (Input(mode_.load())) {
        case Input::LinkIn: {
            auto* b = linkBus_.load();
            const bool live = b && b->linkIn.active.load() && ssbus::nowNs() - b->linkIn.heartbeatNs.load() < 2000000000ull;
            return app_.isEmpty() || !isOn() ? AppCapture::State::Idle : live ? AppCapture::State::Running : AppCapture::State::NotRunning;
        }
        case Input::Link:    return link_.state();
        case Input::Program: break;
    }
    return capture_.state();
}

void AppAudioProcessor::serviceCapture() {
    using S = AppCapture::State;
    if (input() != Input::Program) {
        if ((app_.isEmpty() || !isOn()) && link_.state() != S::Idle && ++offTicks_ > 6) link_.stop();
        else if (isOn() && input() == Input::Link && link_.state() == S::Idle) restartCapture();
        return;
    }
    const S st = capture_.state();
    if (app_.isEmpty() || !isOn()) {
        // let the fade-out finish, then release the capture
        if (st != S::Idle && ++offTicks_ > 6) capture_.stop();
        return;
    }
    offTicks_ = 0;
    if (st == S::Idle) { restartCapture(); return; }
    if (tick_ % 40 != 0) return;   // every 2 s: follow the program (closed, opened later, restarted)
    if (app_ == kSystemAudio) {
        if (st == S::Failed) restartCapture();
    } else if (st == S::Failed || st == S::NotRunning || !capture_.targetAlive()) {
        // only now take a process snapshot; a running capture is checked through its handle
        const uint32_t root = AppCapture::findRootProcess(app_.toStdString());
        if (root != 0 || st != S::NotRunning) restartCapture();
    }
}

void AppAudioProcessor::setParamPlain(const char* id, float plain) {
    if (auto* p = apvts_.getParameter(id)) {
        const float norm = p->convertTo0to1(plain);
        if (std::abs(norm - p->getValue()) < 1.0e-6f) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(norm);   // host sees it: undo, automation, project save
        p->endChangeGesture();
    }
}

void AppAudioProcessor::setOn(bool on) { setParamPlain(appparam::On, on ? 1.0f : 0.0f); }
void AppAudioProcessor::setLevelDb(float db) { setParamPlain(appparam::Level, juce::jlimit(-60.0f, 12.0f, db)); }

void AppAudioProcessor::startRecording() {
    if (manualRec_.load()) return;
    if (!rec_.isOpen() && !rec_.open(displayName())) return;
    manualRec_.store(true);
    stateChanged.sendChangeMessage();
}

void AppAudioProcessor::stopRecording() {
    if (!manualRec_.exchange(false) && !rec_.isOpen()) return;
    rec_.close();
    stateChanged.sendChangeMessage();
}

void AppAudioProcessor::setFollowRecord(bool f) {
    if (follow_.exchange(f) == f) return;
    if (!f && !manualRec_.load()) rec_.close();
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
    stateChanged.sendChangeMessage();
}

void AppAudioProcessor::timerCallback() {
    ++tick_;
    serviceCapture();

    // follow the DAW's record button: a take per DAW take. The next take's file is armed ahead, so
    // the DAW's very first recorded block lands in it (an unused armed file is deleted).
    if (follow_.load() && !manualRec_.load()) {
        if (rec_.ended()) rec_.close();
        if (!rec_.isOpen()) rec_.open(displayName());
    }
    const bool recording = isRecording() && rec_.started();
    if (recording != wasRecording_) { wasRecording_ = recording; stateChanged.sendChangeMessage(); }

    if (src_.load() == nullptr && juce::Time::getMillisecondCounter() - lastConnect_ > 2000) connectBus();
    if (pub_.status() != ssengine::TrackPublisher::Status::Connected && juce::Time::getMillisecondCounter() - lastTrackConnect_ > 2000)
        connectTrack();
    publish();
}

void AppAudioProcessor::connectTrack() {
    lastTrackConnect_ = juce::Time::getMillisecondCounter();
    const auto old = uuid_;
    pub_.connect(defaultBusName().toStdString(), uuid_.toStdString(), uint32_t(sampleRate_ + 0.5), 2);
    uuid_ = juce::String(pub_.uuid());
    if (auto* s = pub_.slot()) {
        slotCmdCursor_ = s->cmdWrite.load(std::memory_order_acquire);   // ignore commands meant for a previous owner
        pub_.prepare(uint32_t(sampleRate_ + 0.5), 2);
    }
    if (old.isNotEmpty() && old != uuid_)   // duplicated track got a fresh identity: let the host save it
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

// ---------------------------------------------------------------------------------------------
// Hub link

void AppAudioProcessor::connectBus() {
    lastConnect_ = juce::Time::getMillisecondCounter();
    ssbus::SharedMemory::Status st {};
    auto shm = ssbus::SharedMemory::open(defaultBusName().toStdString(), st);
    if (!shm) return;
    const int idx = ssbus::claimSource(shm->layout());
    if (idx < 0) { retired_.push_back(std::move(shm)); return; }
    shm_ = std::move(shm);
    bus_ = &shm_->layout();
    srcIndex_ = idx;
    auto& s = bus_->sources[idx];
    cmdCursor_ = s.cmdWrite.load(std::memory_order_acquire);   // ignore commands meant for a previous owner
    appSeq_ = s.appSeq.load(std::memory_order_acquire);
    srcRing_.store(&bus_->sourceAudio[idx], std::memory_order_release);
    linkBus_.store(bus_, std::memory_order_release);
    src_.store(&s, std::memory_order_release);
}

void AppAudioProcessor::publish() {
    juce::uint32 colour;
    {
        const juce::ScopedLock sl(hostLock_);
        colour = hostColour_;
    }
    if (auto* slot = pub_.slot()) {
        std::string n, u;
        uint32_t c = 0;
        const auto want = displayName().toStdString();
        if (!ssbus::readSlotIdentity(*slot, n, u, c) || n != want || c != colour) pub_.setIdentity(want, colour);
        ssbus::pollCommands(*slot, slotCmdCursor_, [this](ssbus::ParamId id, float v) { applyTrackCommand(id, v); });
    }
    busSolo_.store(pub_.soloActive(), std::memory_order_relaxed);

    auto* s = src_.load();
    if (s == nullptr) return;
    s->trackSlot.store(pub_.slotIndex(), std::memory_order_relaxed);
    std::string name, app;
    uint32_t col = 0;
    const auto wantName = displayName().toStdString(), wantApp = app_.toStdString();
    if (!ssbus::readSourceIdentity(*s, name, app, col) || name != wantName || app != wantApp || col != colour)
        ssbus::setSourceIdentity(*s, wantName, wantApp, colour);

    const uint32_t flags = (isOn() ? ssbus::kSrcOn : 0u) | (isRecording() ? ssbus::kSrcRecording : 0u)
                         | (follow_.load() ? ssbus::kSrcFollowRec : 0u) | (app_ == kSystemAudio ? ssbus::kSrcSystem : 0u)
                         | (rec_.dropped() ? ssbus::kSrcDropped : 0u);
    s->flags.store(flags, std::memory_order_relaxed);
    s->capture.store(uint32_t(captureState()), std::memory_order_relaxed);
    s->levelBits.store(ssbus::floatBits(level_->load()), std::memory_order_relaxed);
    s->peakBits.store(ssbus::floatBits(peak()), std::memory_order_relaxed);
    s->latencyBits.store(ssbus::floatBits(float(latencyMs())), std::memory_order_relaxed);
    s->recordMs.store(uint32_t((rec_.isOpen() ? rec_.seconds() : rec_.lastSeconds()) * 1000.0), std::memory_order_relaxed);
    s->delayBits.store(ssbus::floatBits(delay_->load()), std::memory_order_relaxed);
    hubMute_.store(s->hubMute.load(std::memory_order_relaxed) != 0 && ssbus::hubAlive(*bus_, 1000000000ull), std::memory_order_relaxed);

    ssbus::pollSourceCommands(*s, cmdCursor_, [this](SourceParam id, float v) { applyCommand(id, v); });
    std::string exe;
    if (ssbus::pollSourceApp(*s, appSeq_, exe)) setApp(juce::String::fromUTF8(exe.c_str()));
}

void AppAudioProcessor::applyCommand(SourceParam id, float v) {
    switch (id) {
        case SourceParam::On:           setOn(v > 0.5f); break;
        case SourceParam::LevelDb:      setLevelDb(v); break;
        case SourceParam::Record:       if (v > 0.5f) startRecording(); else stopRecording(); break;
        case SourceParam::FollowRecord: setFollowRecord(v > 0.5f); break;
        case SourceParam::DelayMs:      setParamPlain(appparam::Delay, juce::jlimit(0.0f, 500.0f, v)); break;
    }
}

// the Hub's row for this App Audio talks to its slot like to a Track's
void AppAudioProcessor::applyTrackCommand(ssbus::ParamId id, float v) {
    using ssbus::ParamId;
    switch (id) {
        case ParamId::Mon:       setParamPlain(appparam::Mon, v > 0.5f ? 1.0f : 0.0f); break;
        case ParamId::Str:       setParamPlain(appparam::Str, v > 0.5f ? 1.0f : 0.0f); break;
        case ParamId::StrGainDb: setParamPlain(appparam::StrGain, juce::jlimit(-60.0f, 12.0f, v)); break;
        case ParamId::MonTrimDb: setParamPlain(appparam::MonTrim, juce::jlimit(-60.0f, 6.0f, v)); break;
        default: break;   // pan / delay / solo / stem: not for program audio
    }
}

// ---------------------------------------------------------------------------------------------
// state

void AppAudioProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts_.copyState();
    state.setProperty("app", app_, nullptr);
    state.setProperty("follow", follow_.load(), nullptr);
    state.setProperty("uuid", uuid_, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void AppAudioProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    apvts_.replaceState(state);   // older (instrument) versions saved only the program
    setFollowRecord(bool(state.getProperty("follow", false)));
    setApp(state.getProperty("app", "").toString());
    if (const juce::String uuid = state.getProperty("uuid", ""); uuid.isNotEmpty() && uuid != uuid_) {
        uuid_ = uuid;
        connectTrack();
    }
}

// =============================================================================================
// Editor: pick the program, switch, level, "program only" for recording with the DAW.

namespace {

// ComboBox that refreshes its items right before it opens (no polling of the audio sessions).
struct ProgramBox : juce::ComboBox {
    std::function<void()> beforePopup;
    void showPopup() override { if (beforePopup) beforePopup(); juce::ComboBox::showPopup(); }
};

class AppAudioEditor : public EditorShell, private juce::Timer {
public:
    explicit AppAudioEditor(AppAudioProcessor& p)
        : EditorShell(p, 460, 610, int(theme::layout::appMinW), int(theme::layout::appMinH), "app", 800, 1400), proc_(p) {
        content_.paintFn = [this](juce::Graphics& g) { paintContent(g); };
        content_.resizedFn = [this] { layout(); };
        body_.paintFn = [this](juce::Graphics& g) { paintBody(g); };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &power_, &apps_, &meter_, &bodyView_ })
            content_.addAndMakeVisible(c);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &level_, &delay_, &only_, &otherDaws_ })
            body_.addAndMakeVisible(c);
        bodyView_.setViewedComponent(&body_, false);
        bodyView_.setScrollBarsShown(true, false);
        otherDaws_.onClick = [this] { allDaws_ = !allDaws_; refreshTexts(); layout(); content_.repaint(); };
        // viewers delay (the Hub's auto sync sets it), like HEARASIDE Track's
        delay_.setRange(0.0, 500.0, 1.0);
        delay_.setSkewFactorFromMidPoint(100.0);
        delay_.setDoubleClickReturnValue(true, 0.0);
        delay_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        delayAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.params().getParameter(appparam::Delay), delay_);
        levelValue_ = std::make_unique<ValueField>(*proc_.params().getParameter(appparam::Level), ValueField::Unit::Db);
        delayValue_ = std::make_unique<ValueField>(*proc_.params().getParameter(appparam::Delay), ValueField::Unit::Ms);
        body_.addAndMakeVisible(*levelValue_);
        body_.addAndMakeVisible(*delayValue_);
        apps_.beforePopup = [this] { refreshList(); };
        apps_.onChange = [this] {
            if (apps_.getSelectedId() == kAskLink) { askLink(); return; }
            const int i = apps_.getSelectedId() - 1;
            if (i >= 0 && i < int(list_.size())) proc_.setApp(juce::String(list_[size_t(i)].exe));
        };
        power_.onClick = [this] { proc_.setOn(!proc_.isOn()); timerCallback(); };
        only_.onClick = [this] {
            auto* p = proc_.params().getParameter(appparam::Only);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
            p->endChangeGesture();
            timerCallback();
        };
        levelLink_ = std::make_unique<DbSliderLink>(level_, *proc_.params().getParameter(appparam::Level));
        procListener_.fn = [this] { refreshList(); timerCallback(); content_.repaint(); };
        proc_.stateChanged.addChangeListener(&procListener_);
        refreshTexts();
        refreshList();
        setContent(content_);
        timerCallback();
        startTimerHz(15);
    }

    ~AppAudioEditor() override { proc_.stateChanged.removeChangeListener(&procListener_); }

private:
    struct Content : juce::Component {
        std::function<void(juce::Graphics&)> paintFn;
        std::function<void()> resizedFn;
        void paint(juce::Graphics& g) override { if (paintFn) paintFn(g); }
        void resized() override { if (resizedFn) resizedFn(); }
    };

    void refreshTexts() {
        power_.setTooltip(tr(Str::PowerTip));
        power_.setTitle(tr(Str::PowerTip));
        only_.setTooltip(tr(Str::AppOnlyTip));
        delay_.setTitle(tr(Str::ViewersDelay));
        delay_.setTooltip(tr(Str::DelayTip));
        only_.setTitle(tr(Str::AppOnly));
        level_.setTitle(tr(Str::AppLevel));
        otherDaws_.setButtonText(allDaws_ ? tr(Str::StepsThisDaw) : tr(Str::StepsOtherDaws));
        otherDaws_.setVisible(currentDaw() != Daw::Other);
    }

    // How to record the program with the DAW's own Record button: the steps for the DAW in use
    // (all of them when it is unknown, or on request).
    juce::String hintText() const {
        const auto daw = currentDaw();
        juce::StringArray parts { tr(Str::AppHint) };
        auto add = [&](Daw d, Str steps) { if (allDaws_ || daw == Daw::Other || daw == d) parts.add(tr(steps)); };
        add(Daw::StudioOne, Str::AppStepsStudioOne);
        add(Daw::Cubase, Str::AppStepsCubase);
        add(Daw::Reaper, Str::AppStepsReaper);
        if (!allDaws_ && daw != Daw::StudioOne && daw != Daw::Cubase && daw != Daw::Reaper && daw != Daw::Other) parts.add(tr(Str::AppStepsOther));
        parts.add(tr(Str::AppHintNormal));
        return parts.joinIntoString("\n\n");
    }

    void refreshList() {
        list_ = AppCapture::listAudioApps();
        list_.insert(list_.begin(), { AppAudioProcessor::kSystemAudio, {} });
        const juce::String cur = proc_.app();
        const bool curIsLink = cur == AppAudioProcessor::kLinkIn || LinkReceiver::isLink(cur.toStdString());
        bool found = false;
        for (const auto& a : list_) found = found || juce::String(a.exe).equalsIgnoreCase(cur);
        if (cur.isNotEmpty() && !found && !curIsLink) list_.insert(list_.begin() + 1, { cur.toStdString(), {} });
        const size_t programs = list_.size();
        list_.push_back({ AppAudioProcessor::kLinkIn, {} });
        if (LinkReceiver::isLink(cur.toStdString())) list_.push_back({ cur.toStdString(), {} });
        apps_.clear(juce::dontSendNotification);
        apps_.setTextWhenNothingSelected(tr(Str::AppPick));
        for (size_t i = 0; i < list_.size(); ++i) {
            if (i == 1 || i == programs) apps_.addSeparator();
            apps_.addItem(AppAudioProcessor::appLabel(juce::String(list_[i].exe)), int(i) + 1);
            if (juce::String(list_[i].exe).equalsIgnoreCase(cur)) apps_.setSelectedId(int(i) + 1, juce::dontSendNotification);
        }
        apps_.addItem(tr(Str::AppLinkAsk), kAskLink);
    }

    void askLink() {
        auto* aw = new juce::AlertWindow(tr(Str::AppLinkAsk), tr(Str::AppLinkPrompt), juce::MessageBoxIconType::NoIcon, this);
        aw->setLookAndFeel(&lnf_);
        aw->addTextEditor("url", juce::SystemClipboard::getTextFromClipboard().trim().startsWith("http") ? juce::SystemClipboard::getTextFromClipboard().trim()
                                                                                                         : juce::String(), "https://");
        aw->addButton(tr(Str::Done), 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton(tr(Str::Cancel), 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<AppAudioEditor> self(this);
        aw->enterModalState(true, juce::ModalCallbackFunction::create([aw, self](int result) {
            if (self == nullptr) return;
            const auto url = aw->getTextEditorContents("url").trim();
            if (result == 1 && LinkReceiver::isLink(url.toStdString())) self->proc_.setApp(url);
            else if (result == 1) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, tr(Str::AppLinkAsk), tr(Str::AppLinkBad));
            self->refreshList();
        }), true);
    }

    void layout() {
        auto r = content_.getLocalBounds().toFloat().reduced(content_.getWidth() < 420 ? 16.0f : 24.0f);
        auto top = r.removeFromTop(44.0f);
        power_.setBounds(top.removeFromRight(50.0f).withSizeKeepingCentre(50.0f, 30.0f).toNearestInt());
        title_ = top;
        r.removeFromTop(14.0f);
        apps_.setBounds(r.removeFromTop(36.0f).toNearestInt());
        r.removeFromTop(12.0f);
        status_ = r.removeFromTop(20.0f);
        r.removeFromTop(8.0f);
        meter_.setBounds(r.removeFromTop(6.0f).toNearestInt());
        r.removeFromTop(18.0f);
        // below: levels, then recording with the DAW; scrolls when the window is short
        bodyView_.setBounds(r.toNearestInt());
        const float fullW = r.getWidth();
        float w = fullW;
        float h = layoutBody(w);
        if (h > r.getHeight()) { w = fullW - float(bodyView_.getScrollBarThickness() + 4); h = layoutBody(w); }
        body_.setSize(juce::roundToInt(w), juce::roundToInt(juce::jmax(h, r.getHeight())));
    }

    // Lays the body out at width w; returns its height.
    float layoutBody(float w) {
        auto r = juce::Rectangle<float>(0.0f, 0.0f, w, 10000.0f);
        levelLabel_ = r.removeFromTop(18.0f);
        r.removeFromTop(4.0f);
        level_.setBounds(r.removeFromTop(22.0f).toNearestInt());
        r.removeFromTop(14.0f);
        delayLabel_ = r.removeFromTop(18.0f);
        r.removeFromTop(4.0f);
        delay_.setBounds(r.removeFromTop(22.0f).toNearestInt());
        if (levelValue_) {
            levelValue_->setBounds(levelLabel_.withLeft(levelLabel_.getRight() - 80.0f).toNearestInt());
            delayValue_->setBounds(delayLabel_.withLeft(delayLabel_.getRight() - 80.0f).toNearestInt());
        }
        r.removeFromTop(20.0f);
        divider_ = r.removeFromTop(1.0f);
        r.removeFromTop(16.0f);
        recordTitle_ = r.removeFromTop(22.0f);
        r.removeFromTop(10.0f);
        auto row = r.removeFromTop(40.0f);
        only_.setBounds(row.removeFromRight(50.0f).withSizeKeepingCentre(50.0f, 30.0f).toNearestInt());
        row.removeFromRight(12.0f);
        onlyText_ = row;
        r.removeFromTop(12.0f);
        hint_ = r.removeFromTop(wrappedHeight(uiFont(12.0f), hintText(), w, 2.0f) + 4.0f);
        if (otherDaws_.isVisible()) {
            r.removeFromTop(8.0f);
            const float bw = textWidth(uiFont(13.0f, Weight::Medium), otherDaws_.getButtonText()) + 28.0f;
            otherDaws_.setBounds(r.removeFromTop(theme::layout::minTarget).withWidth(juce::jmin(bw, w)).toNearestInt());
        }
        return r.getY() + 4.0f;
    }

    void paintContent(juce::Graphics& g) {
        const auto& p = lnf_.pal();
        g.fillAll(p.paper);
        auto t = title_;
        drawWordmark(g, t.removeFromTop(26.0f).withWidth(220.0f), "APP AUDIO", 15.0f, p.ink);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::AppSubtitle), t, juce::Justification::centredLeft, true);

        juce::String st;
        if (!proc_.isOn()) st = tr(Str::AppOff);
        else {
            const bool link = proc_.input() != AppAudioProcessor::Input::Program;
            switch (proc_.captureState()) {
                case AppCapture::State::Idle:       st = tr(Str::AppNone); break;
                case AppCapture::State::Starting:   st = tr(Str::AppStarting); break;
                case AppCapture::State::Running:    st = link ? tr(Str::AppLinkReceiving) : tr(Str::AppRunning); break;
                case AppCapture::State::NotRunning: st = proc_.input() == AppAudioProcessor::Input::LinkIn ? tr(Str::AppLinkWaiting)
                                                       : link ? tr(Str::AppLinkOffline) : tr(Str::AppNotRunning); break;
                case AppCapture::State::Failed:     st = link ? tr(Str::AppLinkOffline) : tr(Str::AppFailed); break;
            }
        }
        auto s = status_;
        if (proc_.isOn() && proc_.captureState() == AppCapture::State::Running && proc_.latencyMs() > 0.0) {
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            const auto d = tr(Str::AppDelay) + " ~" + juce::String(juce::roundToInt(proc_.latencyMs())) + " ms";
            g.drawText(d, s.removeFromRight(textWidth(uiFont(12.0f), d) + 4.0f), juce::Justification::centredRight, false);
        }
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawFittedText(st, s.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

    }

    void paintBody(juce::Graphics& g) {
        const auto& p = lnf_.pal();
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::AppLevel), levelLabel_.withTrimmedRight(84.0f), juce::Justification::centredLeft, true);
        g.drawText(tr(Str::ViewersDelay), delayLabel_.withTrimmedRight(84.0f), juce::Justification::centredLeft, true);   // values: typeable fields

        g.setColour(p.hairline2);
        g.fillRect(divider_);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::AppRecordTitle), recordTitle_, juce::Justification::centredLeft, true);
        g.setFont(uiFont(13.0f));
        g.drawFittedText(tr(Str::AppOnly), onlyText_.toNearestInt(), juce::Justification::centredLeft, 2, 0.8f);
        drawWrapped(g, hintText(), uiFont(12.0f), p.graphite, hint_, 2.0f);
    }

    void timerCallback() override {
        meter_.setLevel(meterPosition(proc_.peak()));
        power_.setOn(proc_.isOn(), content_.isShowing());
        only_.setOn(proc_.params().getParameter(appparam::Only)->getValue() > 0.5f, content_.isShowing());
        levelLink_->update();
        levelValue_->update();
        delayValue_->update();
        for (auto* f : { levelValue_.get(), delayValue_.get() }) f->setColour(juce::Label::textColourId, lnf_.pal().ink);
        const auto key = std::make_tuple(proc_.isOn(), int(proc_.captureState()), juce::roundToInt(proc_.latencyMs()),
                                         juce::roundToInt(level_.getValue() * 10.0), juce::roundToInt(delay_.getValue()));
        if (key != lastKey_) { lastKey_ = key; content_.repaint(); body_.repaint(); }
    }

    void lookChanged() override { refreshTexts(); refreshList(); layout(); content_.repaint(); body_.repaint(); }

    static constexpr int kAskLink = 10000;   // combo id of "receive someone's link..."
    AppAudioProcessor& proc_;
    Content content_, body_;
    juce::Viewport bodyView_;
    juce::TextButton otherDaws_;
    bool allDaws_ = false;
    Switch power_, only_;
    ProgramBox apps_;
    MeterBar meter_ { 6.0f };
    LevelSlider level_;
    juce::Slider delay_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<juce::SliderParameterAttachment> delayAttachment_;
    std::unique_ptr<ValueField> levelValue_, delayValue_;   // the values, typeable
    std::unique_ptr<DbSliderLink> levelLink_;
    FnChangeListener procListener_;
    std::vector<AppInfo> list_;
    juce::Rectangle<float> title_, status_, levelLabel_, delayLabel_, divider_, recordTitle_, onlyText_, hint_;
    std::tuple<bool, int, int, int, int> lastKey_ {};
};

} // namespace

juce::AudioProcessorEditor* AppAudioProcessor::createEditor() { return new AppAudioEditor(*this); }

} // namespace hearaside

#ifndef HEARASIDE_NO_PLUGIN_ENTRY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hearaside::AppAudioProcessor(); }
#endif
