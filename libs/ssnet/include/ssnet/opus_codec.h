#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace ssnet {

class OpusEnc {
public:
    enum class App {
        Voip,         // Best for speech and singing clarity
        Audio,        // General audio / music
        LowDelay      // Restricted low delay (no speech-mode lookahead)
    };

    OpusEnc();
    ~OpusEnc();

    OpusEnc(OpusEnc&& other) noexcept;
    OpusEnc& operator=(OpusEnc&& other) noexcept;

    OpusEnc(const OpusEnc&) = delete;
    OpusEnc& operator=(const OpusEnc&) = delete;

    // Initialize or reinitialize the encoder.
    bool init(int sampleRate = 48000, int channels = 1, App app = App::Voip);

    // Resets encoder state (clears past history / predictors).
    void reset();

    // Configuration
    void setBitrate(int bitrateBps);
    void setComplexity(int complexity);       // 0..10 (default 5 for fast real-time)
    void setInbandFec(bool enabled);          // enable in-band FEC
    void setPacketLossPerc(int perc);         // 0..100% expected packet loss
    void setVbr(bool vbr);

    // Encode planar audio channels (ch[0], ch[1]). Returns bytes written, or < 0 on error.
    int encode(const float* const* ch, int frames, uint8_t* out, int maxBytes);

    // Encode interleaved float audio. Returns bytes written, or < 0 on error.
    int encodeInterleaved(const float* pcm, int frames, uint8_t* out, int maxBytes);

    int sampleRate() const noexcept { return sampleRate_; }
    int channels() const noexcept { return channels_; }
    bool isValid() const noexcept { return enc_ != nullptr; }

private:
    OpusEncoder* enc_ = nullptr;
    int sampleRate_ = 48000;
    int channels_ = 1;
    std::vector<float> scratch_;   // for interleaving planar input
};

class OpusDec {
public:
    OpusDec();
    ~OpusDec();

    OpusDec(OpusDec&& other) noexcept;
    OpusDec& operator=(OpusDec&& other) noexcept;

    OpusDec(const OpusDec&) = delete;
    OpusDec& operator=(const OpusDec&) = delete;

    // Initialize or reinitialize the decoder.
    bool init(int sampleRate = 48000, int channels = 1);

    // Resets decoder state (clears past history / predictors).
    void reset();

    // Decode Opus packet to planar channels (ch[0], ch[1]).
    // If data == nullptr or bytes == 0, runs PLC (Packet Loss Concealment) for maxFrames.
    // decodeFec = true decodes forward error correction data from packet if available.
    // Returns decoded frames per channel, or < 0 on error.
    int decode(const uint8_t* data, int bytes, float* const* ch, int maxFrames, bool decodeFec = false);

    // Decode Opus packet to interleaved floats.
    // If data == nullptr or bytes == 0, runs PLC.
    int decodeInterleaved(const uint8_t* data, int bytes, float* pcm, int maxFrames, bool decodeFec = false);

    // Helper for explicit packet loss concealment (PLC).
    int decodePlc(float* const* ch, int frames) {
        return decode(nullptr, 0, ch, frames, false);
    }

    int decodePlcInterleaved(float* pcm, int frames) {
        return decodeInterleaved(nullptr, 0, pcm, frames, false);
    }

    int sampleRate() const noexcept { return sampleRate_; }
    int channels() const noexcept { return channels_; }
    bool isValid() const noexcept { return dec_ != nullptr; }

private:
    OpusDecoder* dec_ = nullptr;
    int sampleRate_ = 48000;
    int channels_ = 1;
    std::vector<float> scratch_;   // for de-interleaving to planar output
};

} // namespace ssnet
