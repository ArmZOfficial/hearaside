// Receives another HEARASIDE Hub's "listen" link (like LISTENTO Receiver) for App Audio: a
// WebSocket client (WinHTTP, so https links through Cloudflare work too) that writes the 16-bit PCM
// it gets into a ring the plug-in's audio thread reads. Reconnects by itself.
#pragma once

#include "AppCapture.h"

namespace hearaside {

class LinkReceiver {
public:
    LinkReceiver();
    ~LinkReceiver();

    // "https://xxxx.trycloudflare.com/l/<token>", "http://192.168.1.5:47810/l/<token>" or a permanent
    // link "https://<share site>/l/<token>" (asks the site where the Hub is before every connect)
    static bool isLink(const std::string& text);
    void start(const std::string& url);   // message thread
    void stop();
    AppCapture::State state() const noexcept { return state_.load(std::memory_order_acquire); }

    // audio thread
    const ssbus::ChannelRing& ring() const noexcept { return *ring_; }
    const std::atomic<uint64_t>& writePos() const noexcept { return writePos_; }
    uint32_t sampleRate() const noexcept { return rate_.load(std::memory_order_relaxed); }

private:
    void run(std::string url);

    std::unique_ptr<ssbus::ChannelRing> ring_;
    std::atomic<uint64_t> writePos_{ 0 };
    std::atomic<uint32_t> rate_{ 48000 };
    std::atomic<AppCapture::State> state_{ AppCapture::State::Idle };
    std::atomic<bool> quit_{ false };
    std::atomic<void*> request_{ nullptr };   // open WinHTTP WebSocket handle, closed by stop()
    std::thread thread_;
};

} // namespace hearaside
