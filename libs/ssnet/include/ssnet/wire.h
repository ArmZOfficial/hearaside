#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ssnet {

// HRA wire magic values
constexpr uint32_t kMagic1 = 0x31415248;   // "HRA1" (16 bytes, PCM16)
constexpr uint32_t kMagic2 = 0x32415248;   // "HRA2" (24 bytes, PCM16 + timeline pos)
constexpr uint32_t kMagic3 = 0x33415248;   // "HRA3" (22 bytes, Opus/PCM + seq + pos)

// Codec identifiers for HRA3
enum Codec : uint8_t {
    CodecPcm16 = 0,
    CodecOpus  = 1,
};

// Flags for HRA3
enum Flags : uint16_t {
    FlagNone = 0,
    FlagFec  = 1 << 0,   // packet contains in-band FEC
    FlagDtx  = 1 << 1,   // discontinuous transmission (silence)
};

#pragma pack(push, 1)
// HRA3 header (22 bytes):
// magic "HRA3" (4B) · codec (1B) · chans (1B) · flags (2B) · seq (4B) · pos (8B) · frames (2B)
struct Hra3Header {
    uint32_t magic;    // kMagic3 (0x33415248)
    uint8_t  codec;    // Codec::CodecOpus or CodecPcm16
    uint8_t  chans;    // 1 = mono, 2 = stereo
    uint16_t flags;    // Flags
    uint32_t seq;      // incrementing sequence number
    uint64_t pos;      // Stream Mix timeline frame position / echo timestamp
    uint16_t frames;   // frames per channel in this packet (e.g. 480 for 10ms at 48k)
};
#pragma pack(pop)

static_assert(sizeof(Hra3Header) == 22, "Hra3Header must be exactly 22 bytes");

inline bool parseHra3(const void* data, size_t size, Hra3Header& outHdr, const uint8_t*& outPayload, size_t& outPayloadSize) {
    if (size < sizeof(Hra3Header)) return false;
    std::memcpy(&outHdr, data, sizeof(Hra3Header));
    if (outHdr.magic != kMagic3) return false;
    if (outHdr.chans != 1 && outHdr.chans != 2) return false;
    outPayload = static_cast<const uint8_t*>(data) + sizeof(Hra3Header);
    outPayloadSize = size - sizeof(Hra3Header);
    return true;
}

// Packets missing between the last played seq and this one (survives the u32 wrap). Below 0 = late or repeated:
// over the unordered DataChannel such a packet's place has already been played, so it is dropped.
inline int32_t seqGap(uint32_t seq, uint32_t lastSeq) noexcept { return int32_t(seq - lastSeq) - 1; }

inline void writeHra3(void* dest, const Hra3Header& hdr, const void* payload, size_t payloadSize) {
    std::memcpy(dest, &hdr, sizeof(Hra3Header));
    if (payload != nullptr && payloadSize > 0)
        std::memcpy(static_cast<uint8_t*>(dest) + sizeof(Hra3Header), payload, payloadSize);
}

} // namespace ssnet
