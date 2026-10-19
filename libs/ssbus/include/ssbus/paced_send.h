// What the share server sends to one slow connection: a queue that holds at most what the socket has not
// taken yet, and the rule for where a ring reader continues when the line could not keep up.
// Not for the audio thread (the share server's own threads); no JUCE, no sockets in here.
// SPDX-License-Identifier: MIT
#pragma once

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

    // A control message (ping, pong): always kept, in order, after what waits already.
    void append(const void* p, size_t n) {
        if (idle()) { buf_.clear(); off_ = 0; }
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    // The same, but only while less than `cap` bytes wait: a peer that keeps asking for answers and never reads
    // them must not make this grow. false = not kept.
    bool appendIfRoom(const void* p, size_t n, size_t cap) {
        if (pending() >= cap) return false;
        append(p, n);
        return true;
    }
    // Audio: kept only when nothing waits. false = dropped.
    bool offer(const void* p, size_t n) {
        if (!idle()) return false;
        append(p, n);
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
    size_t off_ = 0;
};

// Where to read next from a ring that `write` has run ahead of: more than maxLagMs behind = skip to keepMs
// before the newest sample (the oldest audio is dropped), else stay.
inline uint64_t catchUp(uint64_t cursor, uint64_t write, uint32_t rate, uint32_t maxLagMs, uint32_t keepMs) noexcept {
    if (write < cursor || write - cursor <= uint64_t(rate) * maxLagMs / 1000) return cursor;
    const uint64_t keep = uint64_t(rate) * keepMs / 1000;
    return write > keep ? write - keep : 0;
}

} // namespace ssbus
