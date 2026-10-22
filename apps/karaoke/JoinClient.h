#pragma once

#include "ssbus/bus.h"
#include "ssengine/friend_reader.h"
#include "ssnet/wire.h"
#include "ssnet/opus_codec.h"
#include "ssnet/aec.h"
#include "ssnet/rtc.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hearaside::karaoke {

enum class ConnectionState {
    Disconnected,
    Resolving,
    Connecting,
    Handshaking,
    Live,
    Reconnecting,
    Failed
};

struct RoomInfo {
    juce::String name;
    juce::String host;
    juce::String token;
    float delayMs = -1.0f;
    float rttMs = 0.0f;

    // K2 metrics
    float bufferMs = 0.0f;
    float jitterP95Ms = -1.0f;
    uint32_t underruns = 0;
    float concealedMs = 0.0f;
    juce::String codec; // "opus" or "pcm16"
    bool speakerMode = false;
    float erleDb = 0.0f;
    bool leakDetected = false;

    // K5 WebRTC transport
    bool isUdp = false;
    juce::String transport; // "WebRTC UDP", "WebSocket Opus", "WebSocket PCM"
};

class JoinClient {
public:
    JoinClient();
    ~JoinClient();

    void prepare(double sampleRate, int maxBlockSize);
    void connect(const juce::String& urlOrToken);
    void disconnect();

    ConnectionState state() const noexcept { return state_.load(std::memory_order_relaxed); }
    bool isLive() const noexcept { return state_.load(std::memory_order_relaxed) == ConnectionState::Live; }
    bool isUdp() const noexcept { return isUdp_.load(std::memory_order_relaxed); }
    RoomInfo roomInfo() const;

    // Speaker mode & AEC control
    void setSpeakerMode(bool enabled);
    bool isSpeakerMode() const noexcept { return speakerMode_.load(std::memory_order_relaxed); }
    void setLocalLatencyMs(double totalLatencyMs);

    // Audio thread: strictly 0 allocations, 0 locks
    // Pulls the room's music + other friends mix into outL, outR
    bool pullDownstream(float* outL, float* outR, int numSamples) noexcept;

    // Pushes singer's microphone audio to be sent to Hub
    void pushUpstream(const float* micSamples, int numSamples, uint64_t playbackPos) noexcept;

    std::function<void(ConnectionState, const RoomInfo&)> onStateChanged;

private:
    void run(juce::String targetUrl);
    void setState(ConnectionState s);

    double sampleRate_ = 48000.0;
    std::atomic<ConnectionState> state_{ ConnectionState::Disconnected };
    std::atomic<bool> quit_{ false };
    std::atomic<void*> wsHandle_{ nullptr };

    // Room info
    mutable std::mutex infoMutex_;
    RoomInfo roomInfo_;
    juce::String activeCodec_{ "pcm16" };

    // Speaker / AEC configuration
    std::atomic<bool> speakerMode_{ false };
    std::atomic<int> localDelaySamples_{ 480 };
    std::mutex aecMutex_;
    ssnet::FriendEcho aec_;

    // Codecs
    std::atomic<bool> isOpus_{ false };
    ssnet::OpusEnc opusEnc_;
    ssnet::OpusDec opusDec_;
    uint32_t upSeq_ = 0;
    uint32_t lastDownSeq_ = 0;
    bool lastDownSeqValid_ = false;

    // Downstream (received from Hub -> FriendReader -> headphones)
    std::unique_ptr<ssbus::ChannelRing> downRing_;
    std::atomic<uint64_t> downWritePos_{ 0 };
    std::atomic<uint32_t> downSampleRate_{ 48000 };
    ssengine::FriendReader friendReader_;
    std::atomic<uint64_t> lastTimelinePos_{ 0 };

    // Upstream (mic -> ring -> sender thread -> Hub)
    std::unique_ptr<ssbus::ChannelRing> upRing_;
    std::atomic<uint64_t> upWritePos_{ 0 };
    uint64_t upReadPos_ = 0;
    std::atomic<uint64_t> lastEchoPos_{ 0 };

    // K5 WebRTC UDP Peer
    std::atomic<bool> isUdp_{ false };
    mutable std::mutex rtcMutex_;
    std::shared_ptr<ssnet::RtcPeer> rtcPeer_;

    std::thread workerThread_;
};

} // namespace hearaside::karaoke
