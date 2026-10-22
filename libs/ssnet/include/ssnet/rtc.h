#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ssnet {

struct IceServerConfig {
    std::string url;
    std::string username;
    std::string password;
};

struct RtcPeerConfig {
    std::vector<IceServerConfig> iceServers;
};

enum class RtcState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed
};

class RtcPeer {
public:
    using DescriptionCallback = std::function<void(const std::string& type, const std::string& sdp)>;
    using CandidateCallback   = std::function<void(const std::string& candidate, const std::string& mid)>;
    using StateCallback       = std::function<void(RtcState state)>;
    using MessageCallback     = std::function<void(const uint8_t* data, size_t size)>;

    explicit RtcPeer(const RtcPeerConfig& config = {});
    ~RtcPeer();

    RtcPeer(const RtcPeer&) = delete;
    RtcPeer& operator=(const RtcPeer&) = delete;
    RtcPeer(RtcPeer&&) noexcept;
    RtcPeer& operator=(RtcPeer&&) noexcept;

    // Signaling & lifecycle callbacks
    void onLocalDescription(DescriptionCallback cb);
    void onLocalCandidate(CandidateCallback cb);
    void onStateChange(StateCallback cb);
    void onDataChannelOpen(std::function<void()> cb);
    void onDataChannelClosed(std::function<void()> cb);
    void onMessage(MessageCallback cb);

    // Remote signaling input
    void setRemoteDescription(const std::string& type, const std::string& sdp);
    void addRemoteCandidate(const std::string& candidate, const std::string& mid);

    // Initiate WebRTC DataChannel (unordered, maxRetransmits=0)
    void initiateOffer(const std::string& channelLabel = "audio");

    // Send binary message (HRA3 audio frame) over DataChannel
    bool send(const uint8_t* data, size_t size);

    bool isOpen() const;
    RtcState state() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssnet
