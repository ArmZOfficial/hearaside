#include "ssnet/opus_codec.h"

#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif

#include <algorithm>
#include <utility>

namespace ssnet {

// ================================================================================================
// OpusEnc
// ================================================================================================

OpusEnc::OpusEnc() = default;

OpusEnc::~OpusEnc() {
    if (enc_ != nullptr) {
        opus_encoder_destroy(enc_);
        enc_ = nullptr;
    }
}

OpusEnc::OpusEnc(OpusEnc&& other) noexcept
    : enc_(std::exchange(other.enc_, nullptr)),
      sampleRate_(other.sampleRate_),
      channels_(other.channels_),
      scratch_(std::move(other.scratch_)) {}

OpusEnc& OpusEnc::operator=(OpusEnc&& other) noexcept {
    if (this != &other) {
        if (enc_ != nullptr) opus_encoder_destroy(enc_);
        enc_ = std::exchange(other.enc_, nullptr);
        sampleRate_ = other.sampleRate_;
        channels_ = other.channels_;
        scratch_ = std::move(other.scratch_);
    }
    return *this;
}

bool OpusEnc::init(int sampleRate, int channels, App app) {
    if (enc_ != nullptr) {
        opus_encoder_destroy(enc_);
        enc_ = nullptr;
    }
    int opusApp = OPUS_APPLICATION_VOIP;
    if (app == App::Audio) opusApp = OPUS_APPLICATION_AUDIO;
    else if (app == App::LowDelay) opusApp = OPUS_APPLICATION_RESTRICTED_LOWDELAY;

    int err = OPUS_OK;
    enc_ = opus_encoder_create(sampleRate, channels, opusApp, &err);
    if (err != OPUS_OK || enc_ == nullptr) {
        enc_ = nullptr;
        return false;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;

    setComplexity(5);
    setInbandFec(true);
    setPacketLossPerc(5);
    setVbr(true);
    return true;
}

void OpusEnc::reset() {
    if (enc_ != nullptr) opus_encoder_ctl(enc_, OPUS_RESET_STATE);
}

void OpusEnc::setBitrate(int bitrateBps) {
    if (enc_ != nullptr && bitrateBps > 0)
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrateBps));
}

void OpusEnc::setComplexity(int complexity) {
    if (enc_ != nullptr)
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(std::clamp(complexity, 0, 10)));
}

void OpusEnc::setInbandFec(bool enabled) {
    if (enc_ != nullptr)
        opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(enabled ? 1 : 0));
}

void OpusEnc::setPacketLossPerc(int perc) {
    if (enc_ != nullptr)
        opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(std::clamp(perc, 0, 100)));
}

void OpusEnc::setVbr(bool vbr) {
    if (enc_ != nullptr)
        opus_encoder_ctl(enc_, OPUS_SET_VBR(vbr ? 1 : 0));
}

int OpusEnc::encode(const float* const* ch, int frames, uint8_t* out, int maxBytes) {
    if (enc_ == nullptr || ch == nullptr || frames <= 0 || out == nullptr || maxBytes <= 0)
        return -1;

    if (channels_ == 1) {
        if (ch[0] == nullptr) return -1;
        return opus_encode_float(enc_, ch[0], frames, out, maxBytes);
    }

    const size_t total = size_t(frames) * 2;
    if (scratch_.size() < total) scratch_.resize(total);
    const float* l = ch[0];
    const float* r = ch[1] ? ch[1] : ch[0];
    for (int i = 0; i < frames; ++i) {
        scratch_[2 * i] = l ? l[i] : 0.0f;
        scratch_[2 * i + 1] = r ? r[i] : 0.0f;
    }
    return opus_encode_float(enc_, scratch_.data(), frames, out, maxBytes);
}

int OpusEnc::encodeInterleaved(const float* pcm, int frames, uint8_t* out, int maxBytes) {
    if (enc_ == nullptr || pcm == nullptr || frames <= 0 || out == nullptr || maxBytes <= 0)
        return -1;
    return opus_encode_float(enc_, pcm, frames, out, maxBytes);
}

// ================================================================================================
// OpusDec
// ================================================================================================

OpusDec::OpusDec() = default;

OpusDec::~OpusDec() {
    if (dec_ != nullptr) {
        opus_decoder_destroy(dec_);
        dec_ = nullptr;
    }
}

OpusDec::OpusDec(OpusDec&& other) noexcept
    : dec_(std::exchange(other.dec_, nullptr)),
      sampleRate_(other.sampleRate_),
      channels_(other.channels_),
      scratch_(std::move(other.scratch_)) {}

OpusDec& OpusDec::operator=(OpusDec&& other) noexcept {
    if (this != &other) {
        if (dec_ != nullptr) opus_decoder_destroy(dec_);
        dec_ = std::exchange(other.dec_, nullptr);
        sampleRate_ = other.sampleRate_;
        channels_ = other.channels_;
        scratch_ = std::move(other.scratch_);
    }
    return *this;
}

bool OpusDec::init(int sampleRate, int channels) {
    if (dec_ != nullptr) {
        opus_decoder_destroy(dec_);
        dec_ = nullptr;
    }
    int err = OPUS_OK;
    dec_ = opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || dec_ == nullptr) {
        dec_ = nullptr;
        return false;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;
    return true;
}

void OpusDec::reset() {
    if (dec_ != nullptr) opus_decoder_ctl(dec_, OPUS_RESET_STATE);
}

int OpusDec::decode(const uint8_t* data, int bytes, float* const* ch, int maxFrames, bool decodeFec) {
    if (dec_ == nullptr || ch == nullptr || maxFrames <= 0) return -1;

    if (channels_ == 1) {
        if (ch[0] == nullptr) return -1;
        return opus_decode_float(dec_, data, bytes, ch[0], maxFrames, decodeFec ? 1 : 0);
    }

    const size_t total = size_t(maxFrames) * 2;
    if (scratch_.size() < total) scratch_.resize(total);
    const int decoded = opus_decode_float(dec_, data, bytes, scratch_.data(), maxFrames, decodeFec ? 1 : 0);
    if (decoded > 0) {
        float* l = ch[0];
        float* r = ch[1];
        for (int i = 0; i < decoded; ++i) {
            if (l) l[i] = scratch_[2 * i];
            if (r) r[i] = scratch_[2 * i + 1];
        }
    }
    return decoded;
}

int OpusDec::decodeInterleaved(const uint8_t* data, int bytes, float* pcm, int maxFrames, bool decodeFec) {
    if (dec_ == nullptr || pcm == nullptr || maxFrames <= 0) return -1;
    return opus_decode_float(dec_, data, bytes, pcm, maxFrames, decodeFec ? 1 : 0);
}

} // namespace ssnet
