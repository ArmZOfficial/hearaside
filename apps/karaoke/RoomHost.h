#pragma once

#include "ShareServer.h"
#include "ssbus/bus.h"
#include "ssengine/friend_reader.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hearaside::karaoke {

struct HostFriendSlot {
    int slot = -1;
    uint32_t id = 0;
    juce::String name;
    juce::String token;
    bool isLive = false;
    bool isMuted = false;
    float peak = 0.0f;
    float delayMs = -1.0f;
};

class RoomHost {
public:
    RoomHost();
    ~RoomHost();

    void prepare(double sampleRate, int maxBlockSize);

    // Host room lifecycle
    bool startRoom(const juce::String& roomName = "Karaoke Room", int port = 0);
    void stopRoom();
    bool isRoomRunning() const noexcept { return isRunning_.load(std::memory_order_relaxed); }

    // Audio thread: strictly 0 locks, 0 allocations
    void processAudio(const float* vocalL, const float* vocalR,
                      const float* musicL, const float* musicR,
                      float* outHeadphoneL, float* outHeadphoneR,
                      int numSamples) noexcept;

    // Room info & Links
    juce::String roomName() const;
    juce::String listenUrl() const;
    juce::String friendUrl(int slot) const;
    juce::String friendDeepLink(int slot) const;
    int serverPort() const;
    bool hasDawCollision() const noexcept { return dawCollision_.load(std::memory_order_relaxed); }

    // Friend slots management (0..7)
    int friendCount() const;
    HostFriendSlot slotInfo(int slot) const;
    void setSlotMuted(int slot, bool muted);
    void kickSlot(int slot);

    // Master Recording
    bool startRecording(const juce::File& targetWavFile);
    void stopRecording();
    bool isRecording() const noexcept { return isRecording_.load(std::memory_order_relaxed); }
    double recordedSeconds() const;

    std::function<void()> onRoomStateChanged;

private:
    void updateFriendLinks();

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    std::atomic<bool> isRunning_{ false };
    std::atomic<bool> dawCollision_{ false };

    // Shared bus layout - MUST BE HEAP ALLOCATED (20+ MB)
    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::unique_ptr<ssbus::BeaconMap> beacon_;
    std::unique_ptr<ssbus::BusLayout> heapBus_;
    ssbus::BusLayout* bus_ = nullptr;
    uint64_t hubToken_ = 0;
    uint64_t timelineCounter_ = 0;

    // Share server
    ShareServer shareServer_;
    juce::String roomName_{ "Karaoke Room" };
    juce::String listenToken_;
    juce::String sendToken_;
    std::array<juce::String, ssbus::kMaxFriends> friendTokens_;
    std::array<juce::String, ssbus::kMaxFriends> friendNames_;
    std::array<std::atomic<bool>, ssbus::kMaxFriends> slotMuted_{};

    // Friend readers for mixing into host headphones
    std::array<ssengine::FriendReader, ssbus::kMaxFriends> friendReaders_;

    // Recording (Threaded, lock-free on audio thread)
    juce::TimeSliceThread recordThread_{ "HAS Karaoke recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter_;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> liveWriter_{ nullptr };
    std::atomic<bool> recordInUse_{ false };
    std::atomic<bool> isRecording_{ false };
    std::atomic<int64_t> recordedSamples_{ 0 };
    juce::File recordingFile_;

    // Scratch buffers for real-time audio processing
    std::vector<float> scratchStreamL_;
    std::vector<float> scratchStreamR_;
    std::vector<float> scratchFriendL_;
    std::vector<float> scratchFriendR_;
    std::vector<float> monoRef_;
};

} // namespace hearaside::karaoke
