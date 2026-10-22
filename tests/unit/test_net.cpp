#include "testing.h"
#include "ssnet/wire.h"
#include "ssnet/opus_codec.h"

#include <cmath>
#include <vector>

TEST_CASE("HRA3 wire header packing and parsing") {
    ssnet::Hra3Header hdr{};
    hdr.magic = ssnet::kMagic3;
    hdr.codec = ssnet::CodecOpus;
    hdr.chans = 1;
    hdr.flags = ssnet::FlagFec;
    hdr.seq = 1042;
    hdr.pos = 960000ull;
    hdr.frames = 480;

    const uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint8_t packet[sizeof(ssnet::Hra3Header) + sizeof(payload)];
    ssnet::writeHra3(packet, hdr, payload, sizeof(payload));

    ssnet::Hra3Header parsed{};
    const uint8_t* parsedPayload = nullptr;
    size_t parsedPayloadSize = 0;

    REQUIRE(ssnet::parseHra3(packet, sizeof(packet), parsed, parsedPayload, parsedPayloadSize));
    CHECK(parsed.magic == ssnet::kMagic3);
    CHECK(parsed.codec == ssnet::CodecOpus);
    CHECK(parsed.chans == 1);
    CHECK(parsed.flags == ssnet::FlagFec);
    CHECK(parsed.seq == 1042);
    CHECK(parsed.pos == 960000ull);
    CHECK(parsed.frames == 480);
    CHECK(parsedPayloadSize == sizeof(payload));
    CHECK(parsedPayload != nullptr);
    CHECK(std::memcmp(parsedPayload, payload, sizeof(payload)) == 0);

    // Reject bad magic
    packet[0] = 'X';
    CHECK(!ssnet::parseHra3(packet, sizeof(packet), parsed, parsedPayload, parsedPayloadSize));

    // Reject too small
    CHECK(!ssnet::parseHra3(packet, sizeof(ssnet::Hra3Header) - 1, parsed, parsedPayload, parsedPayloadSize));
}

TEST_CASE("Opus mono encode, decode, and SNR") {
    ssnet::OpusEnc enc;
    REQUIRE(enc.init(48000, 1, ssnet::OpusEnc::App::Voip));
    enc.setBitrate(64000);   // 64 kbps mono

    ssnet::OpusDec dec;
    REQUIRE(dec.init(48000, 1));

    constexpr int kFramesPerPacket = 480;   // 10 ms at 48 kHz
    constexpr int kNumPackets = 20;         // 200 ms total
    constexpr int kTotalFrames = kFramesPerPacket * kNumPackets;

    std::vector<float> original(kTotalFrames);
    for (int i = 0; i < kTotalFrames; ++i) {
        // 440 Hz sine wave
        original[size_t(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * float(i) / 48000.0f);
    }

    std::vector<float> decoded(kTotalFrames, 0.0f);
    std::vector<uint8_t> packetBuf(1500);

    for (int p = 0; p < kNumPackets; ++p) {
        const float* inPtr = original.data() + p * kFramesPerPacket;
        const float* const inCh[1] = { inPtr };
        const int bytes = enc.encode(inCh, kFramesPerPacket, packetBuf.data(), int(packetBuf.size()));
        REQUIRE(bytes > 0 && bytes < 300);

        float* outPtr = decoded.data() + p * kFramesPerPacket;
        float* const outCh[1] = { outPtr };
        const int decFrames = dec.decode(packetBuf.data(), bytes, outCh, kFramesPerPacket);
        CHECK(decFrames == kFramesPerPacket);
    }

    // Opus has an internal algorithmic delay (typically ~120 samples / 2.5 ms for VoIP mode).
    // Let's compute best correlation lag to align before measuring SNR.
    int bestLag = 0;
    double maxCorr = -1.0;
    constexpr int kMaxSearch = 240;
    for (int lag = 0; lag < kMaxSearch; ++lag) {
        double corr = 0.0;
        for (int i = 480; i < kTotalFrames - kMaxSearch; ++i) {
            corr += double(original[size_t(i)]) * double(decoded[size_t(i + lag)]);
        }
        if (corr > maxCorr) {
            maxCorr = corr;
            bestLag = lag;
        }
    }
    CHECK(bestLag > 0 && bestLag < 200);   // typical lookahead delay

    // Measure SNR on steady state (packets 5..18)
    double signalPower = 0.0, noisePower = 0.0;
    for (int i = 480 * 4; i < 480 * 18; ++i) {
        const double s = original[size_t(i)];
        const double d = decoded[size_t(i + bestLag)];
        const double diff = s - d;
        signalPower += s * s;
        noisePower += diff * diff;
    }
    REQUIRE(noisePower > 0.0);
    const double snr = 10.0 * std::log10(signalPower / noisePower);
    std::printf("  Opus 64 kbps mono SNR: %.1f dB (lag %d frames)\n", snr, bestLag);
    CHECK(snr > 25.0);   // High quality voice/singing transmission
}

TEST_CASE("Opus PLC (packet loss concealment) on dropped packet") {
    ssnet::OpusEnc enc;
    REQUIRE(enc.init(48000, 1, ssnet::OpusEnc::App::Voip));
    enc.setBitrate(64000);

    ssnet::OpusDec dec;
    REQUIRE(dec.init(48000, 1));

    constexpr int kFrames = 480;
    std::vector<float> pcm(kFrames);
    for (int i = 0; i < kFrames; ++i)
        pcm[size_t(i)] = 0.4f * std::sin(2.0f * 3.14159265f * 500.0f * float(i) / 48000.0f);

    const float* const inCh[1] = { pcm.data() };
    std::vector<uint8_t> pkt1(1500), pkt2(1500);

    const int b1 = enc.encode(inCh, kFrames, pkt1.data(), int(pkt1.size()));
    const int b2 = enc.encode(inCh, kFrames, pkt2.data(), int(pkt2.size()));
    REQUIRE(b1 > 0 && b2 > 0);

    std::vector<float> out1(kFrames), outPlc(kFrames), out2(kFrames);
    float* const ch1[1] = { out1.data() };
    float* const chPlc[1] = { outPlc.data() };
    float* const ch2[1] = { out2.data() };

    // Decode packet 1
    CHECK(dec.decode(pkt1.data(), b1, ch1, kFrames) == kFrames);

    // Simulate dropped packet: call decodePlc
    const int plcDecoded = dec.decodePlc(chPlc, kFrames);
    CHECK(plcDecoded == kFrames);

    // Verify PLC generated smooth audio without NaN, Inf, or sudden extreme jump
    float maxPlc = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
        CHECK(!std::isnan(outPlc[size_t(i)]));
        CHECK(!std::isinf(outPlc[size_t(i)]));
        maxPlc = std::max(maxPlc, std::abs(outPlc[size_t(i)]));
    }
    CHECK(maxPlc > 0.01f);   // PLC maintained energy from packet 1
    CHECK(maxPlc < 1.0f);    // No explosion

    // Check transition from end of packet 1 to start of PLC (no huge click)
    const float step = std::abs(outPlc[0] - out1[kFrames - 1]);
    CHECK(step < 0.3f);

    // Next real packet arrives and decodes normally
    CHECK(dec.decode(pkt2.data(), b2, ch2, kFrames) == kFrames);
}

TEST_CASE("Opus stereo encode and decode with channel separation") {
    ssnet::OpusEnc enc;
    REQUIRE(enc.init(48000, 2, ssnet::OpusEnc::App::Audio));
    enc.setBitrate(128000);   // 128 kbps stereo

    ssnet::OpusDec dec;
    REQUIRE(dec.init(48000, 2));

    constexpr int kFrames = 480 * 10;
    std::vector<float> leftIn(kFrames), rightIn(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        leftIn[size_t(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 300.0f * float(i) / 48000.0f);
        rightIn[size_t(i)] = 0.0f;   // Silence on right channel
    }

    std::vector<float> leftOut(kFrames), rightOut(kFrames);
    std::vector<uint8_t> pkt(1500);

    for (int p = 0; p < 10; ++p) {
        const float* const inCh[2] = { leftIn.data() + p * 480, rightIn.data() + p * 480 };
        const int bytes = enc.encode(inCh, 480, pkt.data(), int(pkt.size()));
        REQUIRE(bytes > 0);

        float* const outCh[2] = { leftOut.data() + p * 480, rightOut.data() + p * 480 };
        const int decFrames = dec.decode(pkt.data(), bytes, outCh, 480);
        CHECK(decFrames == 480);
    }

    // Check energy on left vs right
    double energyL = 0.0, energyR = 0.0;
    for (int i = 480 * 2; i < kFrames; ++i) {
        energyL += leftOut[size_t(i)] * leftOut[size_t(i)];
        energyR += rightOut[size_t(i)] * rightOut[size_t(i)];
    }
    CHECK(energyL > 10.0);
    // Channel separation: right should be much quieter than left (> 20 dB isolation)
    const double isolation = 10.0 * std::log10(energyL / std::max(1e-9, energyR));
    std::printf("  Opus stereo separation: %.1f dB\n", isolation);
    CHECK(isolation > 20.0);
}
