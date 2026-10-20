// What the share server sends to one slow connection: a queue that holds at most what the socket has not
// taken yet, and the rule for where a ring reader continues when the line could not keep up.
// Not for the audio thread (the share server's own threads); no JUCE, no sockets in here.
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ssbus {

// Whole messages go in, bytes go out as the socket takes them. Audio is only accepted while nothing waits,
// so a slow line never makes it grow (what would be late is dropped, the caller jumps ahead in its ring).
class SendQueue {
public:
    bool idle() const noexcept { return off_ >= buf_.size(); }
    size_t pending() const noexcept { return buf_.size() - off_; }
    size_t stored() const noexcept { return buf_.size(); }   // what the queue really holds in memory (tests)
    // bytes waiting that are not the audio packet at the front: what the cap on control messages counts
    size_t controlPending() const noexcept { return buf_.size() - std::max(off_, audioEnd_); }

    // A control message (ping, pong): always kept, in order, after what waits already.
    void append(const void* p, size_t n) {
        if (idle()) { buf_.clear(); off_ = audioEnd_ = 0; }
        else if (off_ > 4096 && off_ * 2 > buf_.size()) {   // what is sent already must not stay: a peer that never lets the queue go idle would grow it without end
            buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(off_));
            audioEnd_ = audioEnd_ > off_ ? audioEnd_ - off_ : 0;
            off_ = 0;
        }
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    // The same, but only while `cap` bytes of control messages do not wait yet: a peer that keeps asking for
    // answers and never reads them must not make this grow. false = not kept.
    bool appendIfRoom(const void* p, size_t n, size_t cap) {
        if (controlPending() >= cap) return false;
        append(p, n);
        return true;
    }
    // Audio: kept only when nothing waits. false = dropped.
    bool offer(const void* p, size_t n) {
        if (!idle()) return false;
        append(p, n);
        audioEnd_ = buf_.size();
        return true;
    }
    // send(const uint8_t* p, size_t n) -> bytes taken, 0 = the socket is full right now, < 0 = the connection is gone.
    // Returns false when it is gone. Stops at the first "full".
    template <class Send>
    bool flush(Send&& send) {
        while (!idle()) {
            const auto w = send(buf_.data() + off_, pending());
            if (w < 0) return false;
            if (w == 0) return true;
            off_ += size_t(w);
        }
        return true;
    }

private:
    std::vector<uint8_t> buf_;
    size_t off_ = 0, audioEnd_ = 0;   // audioEnd_: where the audio packet at the front ends
};

// Where to read next from a ring that `write` has run ahead of: more than maxLagMs behind = skip to keepMs
// before the newest sample (the oldest audio is dropped), else stay.
inline uint64_t catchUp(uint64_t cursor, uint64_t write, uint32_t rate, uint32_t maxLagMs, uint32_t keepMs) noexcept {
    if (write < cursor || write - cursor <= uint64_t(rate) * maxLagMs / 1000) return cursor;
    const uint64_t keep = uint64_t(rate) * keepMs / 1000;
    return write > keep ? write - keep : 0;
}

} // namespace ssbus
