#include "TakeRecorder.h"

#include <thread>

namespace hearaside {

namespace {

// Rewrites the BWF TimeReference of a finished WAV file (bext chunk, byte 338 of its data).
void setTimeReference(const juce::File& file, int64_t samples) {
    int64_t at = -1;
    {
        juce::FileInputStream in(file);
        if (!in.openedOk()) return;
        in.setPosition(12);   // "RIFF" size "WAVE" (or RF64)
        while (!in.isExhausted() && at < 0) {
            char id[4];
            if (in.read(id, 4) != 4) return;
            const auto size = int64_t(juce::uint32(in.readInt()));
            if (std::memcmp(id, "bext", 4) == 0 && size >= 346) at = in.getPosition() + 338;
            else in.setPosition(in.getPosition() + size + (size & 1));
        }
    }
    if (at < 0) return;
    juce::FileOutputStream out(file);   // opens without truncating
    if (out.openedOk() && out.setPosition(at)) out.writeInt64(samples);
}

} // namespace

TakeRecorder::TakeRecorder() = default;

TakeRecorder::~TakeRecorder() {
    close();
    thread_.stopThread(2000);
}

juce::File TakeRecorder::folder() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
}

void TakeRecorder::prepare(double sampleRate) {
    close();
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
}

bool TakeRecorder::open(const juce::String& label) {
    close();
    const auto dir = folder();
    if (!dir.createDirectory()) return false;
    const auto now = juce::Time::getCurrentTime();
    label_ = label;
    auto file = dir.getNonexistentChildFile(juce::File::createLegalFileName(label + " " + now.formatted("%Y-%m-%d %H-%M-%S")), ".wav", false);
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) return false;

    const auto bwav = juce::WavAudioFormat::createBWAVMetadata("HEARASIDE App Audio: " + label, "HEARASIDE", {}, now, 0, {});
    auto options = juce::AudioFormatWriterOptions {}
                       .withSampleRate(sampleRate_)
                       .withNumChannels(2)
                       .withBitsPerSample(32)
                       .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    for (int i = 0; i < bwav.size(); ++i) options = options.withMetadata(bwav.getAllKeys()[i], bwav.getAllValues()[i]);
    auto writer = juce::WavAudioFormat().createWriterFor(stream, options);
    if (writer == nullptr) { file.deleteFile(); return false; }

    if (!thread_.isThreadRunning()) thread_.startThread(juce::Thread::Priority::high);
    frames_.store(0);
    start_.store(-1);
    dropped_.store(false);
    phase_.store(0);
    file_ = file;
    // 4 s of headroom for a slow disk before frames are dropped
    writer_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), thread_, int(sampleRate_ * 4.0));
    live_.store(writer_.get());
    return true;
}

void TakeRecorder::close() {
    if (writer_ == nullptr) return;
    live_.store(nullptr);
    while (inUse_.load()) std::this_thread::yield();   // the audio thread finishes its current write
    writer_.reset();                                    // flushes the FIFO and closes the file
    const int64_t frames = frames_.load();
    if (frames == 0) {
        file_.deleteFile();
    } else {
        if (const int64_t start = start_.load(); start >= 0) setTimeReference(file_, start);
        // a take armed ahead (record with the DAW) is named after when it really started
        const auto started = juce::Time::getCurrentTime() - juce::RelativeTime::seconds(double(frames) / sampleRate_);
        const auto name = juce::File::createLegalFileName(label_ + " " + started.formatted("%Y-%m-%d %H-%M-%S"));
        if (!file_.getFileNameWithoutExtension().startsWith(name)) {
            const auto named = file_.getParentDirectory().getNonexistentChildFile(name, ".wav", false);
            if (file_.moveFileTo(named)) file_ = named;
        }
        last_ = file_;
        lastSeconds_ = double(frames) / sampleRate_;
    }
    file_ = {};
    phase_.store(0);
}

void TakeRecorder::write(const float* const* ch, int n, bool want, int64_t timeSamples, bool hasTime) noexcept {
    inUse_.store(true);
    if (auto* w = live_.load()) {
        const int phase = phase_.load(std::memory_order_relaxed);
        if (want && phase != 2) {
            if (phase == 0) {
                const int64_t at = hasTime && timeSamples >= 0 ? timeSamples - shift_.load(std::memory_order_relaxed) : -1;
                start_.store(at >= 0 ? at : (hasTime && timeSamples >= 0 ? 0 : -1), std::memory_order_relaxed);
                phase_.store(1, std::memory_order_release);
            }
            if (!w->write(ch, n)) dropped_.store(true, std::memory_order_relaxed);
            frames_.fetch_add(n, std::memory_order_relaxed);
        } else if (!want && phase == 1) {
            phase_.store(2, std::memory_order_release);
        }
    }
    inUse_.store(false);
}

} // namespace hearaside
