#include "ssnet/rtc.h"

#include <rtc/rtc.hpp>
#include <mutex>
#include <atomic>

namespace ssnet {

static RtcState mapState(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::New:          return RtcState::New;
    case rtc::PeerConnection::State::Connecting:   return RtcState::Connecting;
    case rtc::PeerConnection::State::Connected:    return RtcState::Connected;
    case rtc::PeerConnection::State::Disconnected: return RtcState::Disconnected;
    case rtc::PeerConnection::State::Failed:       return RtcState::Failed;
    case rtc::PeerConnection::State::Closed:       return RtcState::Closed;
    default:                                       return RtcState::Disconnected;
    }
}

struct RtcPeer::Impl {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::mutex mutex;
    std::atomic<bool> isDcOpen{false};
    std::atomic<RtcState> currentState{RtcState::New};

    DescriptionCallback onDesc;
    CandidateCallback onCand;
    StateCallback onState;
    std::function<void()> onDcOpen;
    std::function<void()> onDcClosed;
    MessageCallback onMsg;

    explicit Impl(const RtcPeerConfig& config) {
        rtc::Configuration rtcConfig;
        for (const auto& s : config.iceServers) {
            try {
                rtc::IceServer server(s.url);
                if (!s.username.empty()) server.username = s.username;
                if (!s.password.empty()) server.password = s.password;
                rtcConfig.iceServers.push_back(std::move(server));
            } catch (...) {
                // Ignore invalid ice server URL
            }
        }
        if (config.portRangeBegin != 0) {
            rtcConfig.portRangeBegin = config.portRangeBegin;
            rtcConfig.portRangeEnd = config.portRangeEnd;
        }
        if (rtcConfig.iceServers.empty()) {
            rtcConfig.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
            rtcConfig.iceServers.emplace_back("stun:stun.l.google.com:19302");
        }

        pc = std::make_shared<rtc::PeerConnection>(rtcConfig);

        pc->onLocalDescription([this](rtc::Description desc) {
            DescriptionCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onDesc;
            }
            if (cb) {
                cb(desc.typeString(), std::string(desc));
            }
        });

        pc->onLocalCandidate([this](rtc::Candidate cand) {
            CandidateCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onCand;
            }
            if (cb) {
                cb(cand.candidate(), cand.mid());
            }
        });

        pc->onStateChange([this](rtc::PeerConnection::State state) {
            RtcState st = mapState(state);
            currentState.store(st, std::memory_order_relaxed);
            StateCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onState;
            }
            if (cb) {
                cb(st);
            }
        });

        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> incoming) {
            setupDataChannel(incoming);
        });
    }

    void setupDataChannel(std::shared_ptr<rtc::DataChannel> channel) {
        std::lock_guard<std::mutex> lock(mutex);
        dc = channel;
        if (!dc) return;

        dc->onOpen([this]() {
            isDcOpen.store(true, std::memory_order_release);
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onDcOpen;
            }
            if (cb) cb();
        });

        dc->onClosed([this]() {
            isDcOpen.store(false, std::memory_order_release);
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onDcClosed;
            }
            if (cb) cb();
        });

        dc->onMessage([this](std::variant<rtc::binary, rtc::string> msg) {
            MessageCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cb = onMsg;
            }
            if (cb && std::holds_alternative<rtc::binary>(msg)) {
                const auto& bin = std::get<rtc::binary>(msg);
                if (!bin.empty()) {
                    cb(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
                }
            }
        });
    }
};

RtcPeer::RtcPeer(const RtcPeerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

RtcPeer::~RtcPeer() = default;
RtcPeer::RtcPeer(RtcPeer&&) noexcept = default;
RtcPeer& RtcPeer::operator=(RtcPeer&&) noexcept = default;

void RtcPeer::onLocalDescription(DescriptionCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onDesc = std::move(cb);
}

void RtcPeer::onLocalCandidate(CandidateCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onCand = std::move(cb);
}

void RtcPeer::onStateChange(StateCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onState = std::move(cb);
}

void RtcPeer::onDataChannelOpen(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onDcOpen = std::move(cb);
}

void RtcPeer::onDataChannelClosed(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onDcClosed = std::move(cb);
}

void RtcPeer::onMessage(MessageCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onMsg = std::move(cb);
}

void RtcPeer::setRemoteDescription(const std::string& type, const std::string& sdp) {
    if (!impl_->pc) return;
    try {
        impl_->pc->setRemoteDescription(rtc::Description(sdp, type));
    } catch (...) {
        // Handle invalid remote description gracefully
    }
}

void RtcPeer::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
    if (!impl_->pc) return;
    try {
        impl_->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (...) {
        // Handle invalid remote candidate gracefully
    }
}

void RtcPeer::initiateOffer(const std::string& channelLabel) {
    if (!impl_->pc) return;
    rtc::DataChannelInit init;
    init.reliability.unordered = true;
    init.reliability.maxRetransmits = 0;
    auto dc = impl_->pc->createDataChannel(channelLabel, init);
    impl_->setupDataChannel(dc);
}

bool RtcPeer::send(const uint8_t* data, size_t size) {
    if (!data || size == 0 || !impl_->isDcOpen.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        dc = impl_->dc;
    }
    if (!dc || !dc->isOpen()) return false;
    return dc->send(reinterpret_cast<const rtc::byte*>(data), size);
}

bool RtcPeer::isOpen() const {
    return impl_->isDcOpen.load(std::memory_order_acquire);
}

RtcState RtcPeer::state() const {
    return impl_->currentState.load(std::memory_order_relaxed);
}

void RtcPeer::close() {
    impl_->isDcOpen.store(false, std::memory_order_release);
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        dc = impl_->dc;
        pc = impl_->pc;
    }
    if (dc) {
        try { dc->close(); } catch (...) {}
    }
    if (pc) {
        try { pc->close(); } catch (...) {}
    }
}

} // namespace ssnet
