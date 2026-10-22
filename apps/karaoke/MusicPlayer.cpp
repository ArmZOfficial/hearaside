#include "MusicPlayer.h"

#include <cmath>
#include <algorithm>

namespace hearaside::karaoke {

MusicPlayer::MusicPlayer() {
    formatManager_.registerBasicFormats(); // WAV, AIFF
    formatManager_.registerFormat(new juce::FlacAudioFormat(), true);
    formatManager_.registerFormat(new juce::OggVorbisAudioFormat(), true);
#if JUCE_USE_MP3AUDIOFORMAT
    formatManager_.registerFormat(new juce::MP3AudioFormat(), true);
#endif
#if JUCE_WINDOWS
    formatManager_.registerFormat(new juce::WindowsMediaAudioFormat(), true); // M4A, AAC, WMA
#endif
    transportSource_.addChangeListener(this);
}

MusicPlayer::~MusicPlayer() {
    transportSource_.removeChangeListener(this);
    transportSource_.setSource(nullptr);
    stopAppCapture();
}

void MusicPlayer::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max(maxBlockSize, 256);
    transportSource_.prepareToPlay(maxBlockSize_, sampleRate_);
    scratchBuffer_.setSize(2, std::max(maxBlockSize_, 2048));
    scratchBuffer_.clear();
}

void MusicPlayer::pullAudio(float* outL, float* outR, int numSamples) noexcept {
    if (numSamples <= 0) return;

    const float vol = volumeLinear_.load(std::memory_order_relaxed);

    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        if (transportSource_.isPlaying() && numSamples <= scratchBuffer_.getNumSamples()) {
            juce::AudioSourceChannelInfo info(&scratchBuffer_, 0, numSamples);
            scratchBuffer_.clear(0, numSamples);
            transportSource_.getNextAudioBlock(info);

            const float* srcL = scratchBuffer_.getReadPointer(0);
            const float* srcR = (scratchBuffer_.getNumChannels() > 1) ? scratchBuffer_.getReadPointer(1) : srcL;

            for (int i = 0; i < numSamples; ++i) {
                if (outL != nullptr) outL[i] = srcL[i] * vol;
                if (outR != nullptr) outR[i] = srcR[i] * vol;
            }
            return;
        }
    } else if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AppCapture) {
        if (appCapture_.state() == AppCapture::State::Running) {
            float* dst[2] = { outL, outR };
            const uint64_t w = appCapture_.writePos().load(std::memory_order_acquire);
            if (appReadPos_ == 0 || w < appReadPos_ || w - appReadPos_ > uint64_t(sampleRate_)) {
                appReadPos_ = (w > uint64_t(numSamples * 2)) ? (w - uint64_t(numSamples * 2)) : 0;
            }
            ssbus::ringReadAt(appCapture_.ring(), appCapture_.writePos(), appReadPos_, dst, uint32_t(numSamples));
            appReadPos_ += uint64_t(numSamples);

            for (int i = 0; i < numSamples; ++i) {
                if (outL != nullptr) outL[i] *= vol;
                if (outR != nullptr) outR[i] *= vol;
            }
            return;
        }
    }

    if (outL != nullptr) std::fill_n(outL, numSamples, 0.0f);
    if (outR != nullptr) std::fill_n(outR, numSamples, 0.0f);
}

void MusicPlayer::setSourceMode(MusicSourceMode mode) {
    if (sourceMode_.load(std::memory_order_relaxed) == mode) return;

    if (mode == MusicSourceMode::AudioFile) {
        stopAppCapture();
    } else {
        transportSource_.stop();
    }
    sourceMode_.store(mode, std::memory_order_relaxed);
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

void MusicPlayer::addTrack(const juce::File& file) {
    if (!file.existsAsFile()) return;

    double duration = 0.0;
    if (auto* reader = formatManager_.createReaderFor(file)) {
        duration = double(reader->lengthInSamples) / std::max(1.0, reader->sampleRate);
        delete reader;
    }

    TrackInfo track;
    track.file = file;
    track.title = file.getFileNameWithoutExtension();
    track.durationSec = duration;

    bool selectFirst = false;
    {
        std::lock_guard<std::mutex> lk(playlistMutex_);
        playlist_.push_back(track);
        if (playlist_.size() == 1) selectFirst = true;
    }

    if (selectFirst) {
        selectTrack(0);
    }
    if (onTrackChanged) onTrackChanged();
}

void MusicPlayer::removeTrack(int index) {
    std::lock_guard<std::mutex> lk(playlistMutex_);
    if (index >= 0 && index < int(playlist_.size())) {
        playlist_.erase(playlist_.begin() + index);
        int cur = currentTrackIndex_.load(std::memory_order_relaxed);
        if (cur == index) {
            currentTrackIndex_.store(-1, std::memory_order_relaxed);
            transportSource_.setSource(nullptr);
            readerSource_.reset();
        } else if (cur > index) {
            currentTrackIndex_.store(cur - 1, std::memory_order_relaxed);
        }
    }
    if (onTrackChanged) onTrackChanged();
}

void MusicPlayer::clearPlaylist() {
    stop();
    transportSource_.setSource(nullptr);
    readerSource_.reset();
    {
        std::lock_guard<std::mutex> lk(playlistMutex_);
        playlist_.clear();
    }
    currentTrackIndex_.store(-1, std::memory_order_relaxed);
    trackLengthSec_.store(0.0, std::memory_order_relaxed);
    if (onTrackChanged) onTrackChanged();
}

int MusicPlayer::trackCount() const {
    std::lock_guard<std::mutex> lk(playlistMutex_);
    return int(playlist_.size());
}

TrackInfo MusicPlayer::trackAt(int index) const {
    std::lock_guard<std::mutex> lk(playlistMutex_);
    if (index >= 0 && index < int(playlist_.size())) {
        return playlist_[size_t(index)];
    }
    return {};
}

void MusicPlayer::selectTrack(int index) {
    juce::File f;
    {
        std::lock_guard<std::mutex> lk(playlistMutex_);
        if (index >= 0 && index < int(playlist_.size())) {
            currentTrackIndex_.store(index, std::memory_order_relaxed);
            f = playlist_[size_t(index)].file;
        } else {
            return;
        }
    }

    transportSource_.stop();
    transportSource_.setSource(nullptr);
    readerSource_.reset();

    if (f.existsAsFile()) {
        if (auto* reader = formatManager_.createReaderFor(f)) {
            const double len = double(reader->lengthInSamples) / std::max(1.0, reader->sampleRate);
            trackLengthSec_.store(len, std::memory_order_relaxed);
            readerSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
            transportSource_.setSource(readerSource_.get(), 0, nullptr, reader->sampleRate);
        }
    }

    if (onTrackChanged) onTrackChanged();
}

void MusicPlayer::nextTrack() {
    const int count = trackCount();
    if (count <= 0) return;
    int next = currentTrackIndex_.load(std::memory_order_relaxed) + 1;
    if (next >= count) next = 0;
    selectTrack(next);
    play();
}

void MusicPlayer::prevTrack() {
    const int count = trackCount();
    if (count <= 0) return;
    int prev = currentTrackIndex_.load(std::memory_order_relaxed) - 1;
    if (prev < 0) prev = count - 1;
    selectTrack(prev);
    play();
}

void MusicPlayer::play() {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        if (currentTrackIndex_.load(std::memory_order_relaxed) < 0 && trackCount() > 0) {
            selectTrack(0);
        }
        transportSource_.start();
    }
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

void MusicPlayer::pause() {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        transportSource_.stop();
    }
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

void MusicPlayer::stop() {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        transportSource_.stop();
        transportSource_.setPosition(0.0);
    }
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

bool MusicPlayer::isPlaying() const {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        return transportSource_.isPlaying();
    }
    return appCapture_.state() == AppCapture::State::Running;
}

double MusicPlayer::currentPositionSec() const {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        return transportSource_.getCurrentPosition();
    }
    return 0.0;
}

double MusicPlayer::lengthSeconds() const {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        return trackLengthSec_.load(std::memory_order_relaxed);
    }
    return 0.0;
}

void MusicPlayer::seekTo(double seconds) {
    if (sourceMode_.load(std::memory_order_relaxed) == MusicSourceMode::AudioFile) {
        transportSource_.setPosition(std::max(0.0, seconds));
    }
}

std::vector<AppInfo> MusicPlayer::listAudioApps() {
    return AppCapture::listAudioApps();
}

void MusicPlayer::startAppCapture(uint32_t pid, const juce::String& name) {
    stop();
    {
        std::lock_guard<std::mutex> lk(appMutex_);
        capturedAppName_ = name;
    }
    appReadPos_ = 0;
    appCapture_.start(pid, uint32_t(sampleRate_ > 0 ? sampleRate_ : 48000.0), false);
    sourceMode_.store(MusicSourceMode::AppCapture, std::memory_order_relaxed);
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

void MusicPlayer::stopAppCapture() {
    appCapture_.stop();
    {
        std::lock_guard<std::mutex> lk(appMutex_);
        capturedAppName_ = "";
    }
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

bool MusicPlayer::isAppCapturing() const {
    return appCapture_.state() == AppCapture::State::Running;
}

juce::String MusicPlayer::capturedAppName() const {
    std::lock_guard<std::mutex> lk(appMutex_);
    return capturedAppName_;
}

void MusicPlayer::changeListenerCallback(juce::ChangeBroadcaster*) {
    // End of track detection
    if (transportSource_.hasStreamFinished()) {
        nextTrack();
    }
    if (onPlaybackStateChanged) onPlaybackStateChanged();
}

} // namespace hearaside::karaoke
