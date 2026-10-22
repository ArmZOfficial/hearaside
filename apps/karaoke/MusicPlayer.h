#pragma once

#include "AppCapture.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hearaside::karaoke {

enum class MusicSourceMode {
    AudioFile,
    AppCapture
};

struct TrackInfo {
    juce::File file;
    juce::String title;
    double durationSec = 0.0;
};

class MusicPlayer : private juce::ChangeListener {
public:
    MusicPlayer();
    ~MusicPlayer() override;

    void prepare(double sampleRate, int maxBlockSize);

    // Real-time audio thread: strictly 0 locks, 0 allocations
    void pullAudio(float* outL, float* outR, int numSamples) noexcept;

    // Source selection
    void setSourceMode(MusicSourceMode mode);
    MusicSourceMode sourceMode() const noexcept { return sourceMode_.load(std::memory_order_relaxed); }

    // Playlist
    void addTrack(const juce::File& file);
    void removeTrack(int index);
    void clearPlaylist();
    int trackCount() const;
    TrackInfo trackAt(int index) const;
    int currentTrackIndex() const noexcept { return currentTrackIndex_.load(std::memory_order_relaxed); }
    void selectTrack(int index);
    void nextTrack();
    void prevTrack();

    // Transport controls
    void play();
    void pause();
    void stop();
    bool isPlaying() const;
    double currentPositionSec() const;
    double lengthSeconds() const;
    void seekTo(double seconds);

    void setVolume(float linear) noexcept { volumeLinear_.store(linear, std::memory_order_relaxed); }
    float volume() const noexcept { return volumeLinear_.load(std::memory_order_relaxed); }

    // AppCapture controls (YouTube, Spotify, etc.)
    static std::vector<AppInfo> listAudioApps();
    void startAppCapture(uint32_t pid, const juce::String& name = {});
    void stopAppCapture();
    bool isAppCapturing() const;
    juce::String capturedAppName() const;

    std::function<void()> onTrackChanged;
    std::function<void()> onPlaybackStateChanged;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void loadCurrentTrack();

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    std::atomic<MusicSourceMode> sourceMode_{ MusicSourceMode::AudioFile };
    std::atomic<float> volumeLinear_{ 1.0f };

    // JUCE Audio Format & Transport
    juce::AudioFormatManager formatManager_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    juce::AudioTransportSource transportSource_;
    juce::AudioBuffer<float> scratchBuffer_;

    mutable std::mutex playlistMutex_;
    std::vector<TrackInfo> playlist_;
    std::atomic<int> currentTrackIndex_{ -1 };
    std::atomic<double> trackLengthSec_{ 0.0 };

    // AppCapture (WASAPI process loopback)
    AppCapture appCapture_;
    uint64_t appReadPos_ = 0;
    mutable std::mutex appMutex_;
    juce::String capturedAppName_;
};

} // namespace hearaside::karaoke
