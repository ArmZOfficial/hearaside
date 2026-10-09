// Prints what HEARASIDE App Audio adds to its track into WAV files (32-bit float) that you drag
// into the DAW. The audio thread only pushes into a lock-free FIFO; a background thread writes
// the file. Each take carries a BWF time reference = where it starts on the DAW timeline, so
// "move to origin" (Cubase, Reaper, Pro Tools...) puts it back in place.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>

namespace hearaside {

class TakeRecorder {
public:
    TakeRecorder();
    ~TakeRecorder();

    // ---- message thread -----------------------------------------------------------------
    void prepare(double sampleRate);            // closes any open take
    bool open(const juce::String& label);       // new file; writing starts with the next block that wants it
    void close();                               // finishes the take (an empty one is deleted)
    bool isOpen() const noexcept { return writer_ != nullptr; }
    bool started() const noexcept { return phase_.load(std::memory_order_acquire) >= 1; }
    bool ended() const noexcept { return phase_.load(std::memory_order_acquire) == 2; }   // audio thread stopped it
    double seconds() const noexcept { return double(frames_.load(std::memory_order_relaxed)) / sampleRate_; }
    bool dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    juce::File lastTake() const { return last_; }
    double lastSeconds() const noexcept { return lastSeconds_; }
    static juce::File folder();                 // Documents/HEARASIDE/Recordings

    // ---- audio thread -------------------------------------------------------------------
    // want: this block belongs to the take. A take that started and then gets want = false ends.
    // hasTime = false: no timeline position (transport stopped), the take gets no BWF position.
    void write(const float* const* ch, int n, bool want, int64_t timeSamples, bool hasTime) noexcept;

private:
    juce::TimeSliceThread thread_ { "HEARASIDE take writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer_;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> live_ { nullptr };
    std::atomic<bool> inUse_ { false };
    std::atomic<int> phase_ { 0 };              // 0 waiting, 1 writing, 2 ended
    std::atomic<int64_t> frames_ { 0 };
    std::atomic<int64_t> start_ { -1 };         // timeline sample of the first frame, -1 = unknown
    std::atomic<bool> dropped_ { false };
    double sampleRate_ = 48000.0;
    juce::File file_, last_;
    juce::String label_;
    double lastSeconds_ = 0.0;
};

} // namespace hearaside
