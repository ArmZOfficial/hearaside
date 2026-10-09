// HEARASIDE App Audio: an effect that adds what one program (YouTube in a browser, Spotify, a
// game) - or the whole computer except the DAW - plays to the track it sits on, so it goes
// through the DAW like any other signal. Works on audio, instrument, bus and input channels: on
// an input channel (with "program only") the DAW's own Record button records the program.
// Like HEARASIDE Track it has "you hear" / "viewers hear": the program goes to the DAW (viewers)
// and is published on its own bus slot, from which the Hub puts it in your headphones. The Hub
// lists it for remote control; it can also print the program's audio to WAV takes.
#pragma once

#include "AppCapture.h"
#include "LinkReceiver.h"
#include "Settings.h"
#include "TakeRecorder.h"
#include "ssbus/bus.h"
#include "ssdsp/consumer.h"
#include "ssengine/engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace hearaside {

namespace appparam {
inline constexpr const char* On    = "on";
inline constexpr const char* Level = "level";
inline constexpr const char* Delay = "delay";   // ms, set by the Hub's auto sync
inline constexpr const char* Only  = "only";    // drop the channel's own signal (an input channel the DAW records)
// like HEARASIDE Track: the program in your headphones (through the Hub) and/or to the viewers (the DAW)
inline constexpr const char* Mon     = "mon";
inline constexpr const char* Str     = "str";
inline constexpr const char* StrGain = "strGain";
inline constexpr const char* MonTrim = "monTrim";
}

class AppAudioProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    static constexpr const char* kSystemAudio = "*system*";   // app() value: everything but the DAW
    static constexpr const char* kLinkIn = "*link*";          // app() value: what someone sends through the Hub's send link
    // app() may also be another HEARASIDE Hub's listen link ("https://.../l/<token>")

    AppAudioProcessor();
    ~AppAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HEARASIDE App Audio"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    // never "silent": hosts that suspend effects without input (Cubase) must keep calling us
    double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;

    // ---- message thread -----------------------------------------------------------------
    juce::AudioProcessorValueTreeState& params() noexcept { return apvts_; }
    juce::String app() const { return app_; }   // "chrome.exe", kSystemAudio, "" = none
    void setApp(const juce::String& exe);
    static juce::String appLabel(const juce::String& exe);   // what the UI shows for app()
    juce::String displayName() const;                        // host track name, or the program
    AppCapture::State captureState() const noexcept;   // of the current input (program, send link or listen link)
    enum class Input { Program, LinkIn, Link };
    Input input() const noexcept { return Input(mode_.load()); }
    bool isOn() const noexcept { return on_->load() > 0.5f; }
    void setOn(bool);
    void setLevelDb(float);
    float peak() const noexcept { return peak_.load(std::memory_order_relaxed); }
    double latencyMs() const noexcept { return latencyMs_.load(std::memory_order_relaxed); }
    uint32_t capturePacketFrames() const noexcept { return capture_.packetFrames(); }
    bool captureShortPeriod() const noexcept { return capture_.shortPeriod(); }
    bool connectedToBus() const noexcept { return src_.load() != nullptr; }

    // printing (recording the program's audio to WAV takes)
    void startRecording();
    void stopRecording();
    bool isRecording() const noexcept { return manualRec_.load() || (follow_.load() && rec_.started() && !rec_.ended()); }
    bool followRecord() const noexcept { return follow_.load(); }
    void setFollowRecord(bool);
    const TakeRecorder& recorder() const noexcept { return rec_; }

    juce::ChangeBroadcaster stateChanged;   // program, recording or identity changed

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void restartCapture();
    void serviceCapture();
    void connectBus();
    void connectTrack();   // the headphone slot (TrackPublisher)
    void publish();
    void applyCommand(ssbus::SourceParam id, float v);
    void applyTrackCommand(ssbus::ParamId id, float v);
    void setParamPlain(const char* id, float plain);
    void processChunk(float* const* io, int numCh, int n, int64_t time, bool hasTime, bool hostRecording);
    void finishChunk(float* const* io, int numCh, int n, bool have, int64_t time, bool hasTime, bool hostRecording);
    bool pull(float* const* out, int n);   // the program's audio for this block, false = nothing yet
    struct Feed { const ssbus::ChannelRing* ring; const std::atomic<uint64_t>* writePos; uint32_t rate; double packet; };
    Feed feed() const noexcept;            // audio thread: where the current input's audio is

    juce::AudioProcessorValueTreeState apvts_;
    std::atomic<float>* on_ = nullptr;
    std::atomic<float>* level_ = nullptr;
    std::atomic<float>* delay_ = nullptr;
    std::atomic<float>* only_ = nullptr;
    std::atomic<float>* mon_ = nullptr;
    std::atomic<float>* str_ = nullptr;
    std::atomic<float>* strGain_ = nullptr;
    std::atomic<float>* monTrim_ = nullptr;

    // headphone slot: what you hear comes from here (through the Hub), what viewers hear goes to the DAW
    ssengine::TrackPublisher pub_;
    juce::String uuid_;                       // saved, so the Hub keeps the same slot identity
    uint32_t slotCmdCursor_ = 0;
    juce::uint32 lastTrackConnect_ = 0;
    std::atomic<bool> busSolo_ { false };     // some track is soloed for the viewers
    juce::SmoothedValue<float> viewers_;      // Viewers Hear x Viewers Level

    AppCapture capture_;
    LinkReceiver link_;
    std::atomic<int> mode_ { 0 };                            // Input
    std::atomic<ssbus::BusLayout*> linkBus_ { nullptr };     // for Input::LinkIn
    std::unique_ptr<ssdsp::VarResampler> rs48_, rs441_;      // links from a 48 / 44.1 kHz sender
    uint64_t feedKey_ = 0;                                   // input + rate the buffer was set up for
    juce::String app_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    std::atomic<bool> preparing_ { true };

    // audio thread: ring -> drift-corrected resampler -> track
    std::unique_ptr<ssdsp::VarResampler> rs_;
    ssdsp::DriftController drift_;
    std::vector<float> in_[2], appBuf_[2];
    uint64_t cursor_ = 0, lastWrite_ = 0;
    double sinceWrite_ = 0;     // seconds of DAW audio since the program last delivered a packet
    bool primed_ = false;
    uint64_t starvedAt_ = 0;    // write position when the program last fell silent
    // Buffer target: starts safe (DAW block + one capture packet + 2 ms), then shrinks to what the
    // measured timing needs (down to one DAW block + ~1 ms). An underrun backs off and holds 30 s.
    double targetFrames_ = 960;
    double floorFrames_ = 0;    // never shrink below a level that already ran dry once
    double minMargin_ = 1.0e9, windowSec_ = 0, holdSec_ = 0;
    double fillAvg_ = 0;        // buffered frames, ~0.3 s average: the real delay
    juce::SmoothedValue<float> gain_, fade_;
    std::atomic<float> peak_ { 0.0f };
    std::atomic<double> latencyMs_ { 0.0 };
    uint32_t offTicks_ = 0;

    // printing
    TakeRecorder rec_;
    std::atomic<bool> manualRec_ { false }, follow_ { false };
    bool wasRecording_ = false;

    // Hub link (source entry on the bus)
    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::vector<std::unique_ptr<ssbus::SharedMemory>> retired_;   // the audio thread may still touch them
    std::atomic<ssbus::SourceHeader*> src_ { nullptr };
    std::atomic<ssbus::ChannelRing*> srcRing_ { nullptr };   // our signal, for the Hub's auto sync
    std::atomic<bool> hubMute_ { false };                    // the Hub is measuring
    juce::AudioBuffer<float> delayLine_;                     // sync delay of the program audio
    int delayWrite_ = 0;
    ssbus::BusLayout* bus_ = nullptr;
    int srcIndex_ = -1;
    uint32_t cmdCursor_ = 0, appSeq_ = 0;
    juce::uint32 lastConnect_ = 0;
    int tick_ = 0;

    juce::CriticalSection hostLock_;
    juce::String hostName_;
    juce::uint32 hostColour_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppAudioProcessor)
};

} // namespace hearaside
