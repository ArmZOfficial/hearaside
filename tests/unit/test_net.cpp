#include "testing.h"
#include "ssnet/wire.h"
#include "ssnet/opus_codec.h"
#include "ssnet/aec.h"

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

TEST_CASE("FriendEcho AEC: 120 ms IR with 80 ms delay achieves ERLE >= 25 dB and speech SNR >= 15 dB") {
    ssnet::FriendEcho aec;
    REQUIRE(aec.init(48000, 480, 200));
    aec.setMode(ssnet::AecMode::On);

    constexpr int kSampleRate = 48000;
    constexpr int kFrameSize = 480;
    constexpr int kDelaySamples = 3840;   // 80 ms delay
    constexpr int kIrLen = 5760;          // 120 ms impulse response

    // Synthesize a realistic acoustic room impulse response (early reflections + decaying diffuse tail)
    std::vector<float> ir(kIrLen, 0.0f);
    ir[0] = 0.5f;        // direct speaker path
    ir[240] = -0.25f;    // reflection 1 (~5 ms)
    ir[600] = 0.18f;     // reflection 2 (~12.5 ms)
    ir[1200] = -0.12f;   // reflection 3 (~25 ms)
    uint32_t irSeed = 1234;
    for (int k = 0; k < kIrLen; ++k) {
        irSeed = irSeed * 1664525u + 1013904223u;
        const float n = float(int(irSeed >> 16) - 32768) / 32768.0f;
        const float t = float(k) / float(kSampleRate);
        ir[size_t(k)] += 0.04f * std::exp(-t / 0.030f) * n;
    }
    double irEnergy = 0.0;
    for (float v : ir) irEnergy += v * v;
    const float irScale = float(std::sqrt(0.15 / irEnergy));   // ERL ~ -8.2 dB
    for (float& v : ir) v *= irScale;

    // Generate 5 seconds of far-end reference signal (wideband audio/music) with pre-roll
    constexpr int kNumFrames = 500;   // 5.0 seconds
    constexpr int kTotalSamples = kNumFrames * kFrameSize;
    constexpr int kPreRoll = kDelaySamples + kIrLen + 480;
    std::vector<float> ref(kTotalSamples + kPreRoll, 0.0f);

    uint32_t seed = 42;
    for (size_t i = 0; i < ref.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        ref[i] = 0.4f * (float(int(seed >> 16) - 32768) / 32768.0f);
    }

    // Convolve reference with delayed room IR to simulate acoustic speaker leak into microphone.
    // At sample i, the speaker played the reference at (kPreRoll + i - kDelaySamples).
    std::vector<float> micEcho(kTotalSamples, 0.0f);
    for (int i = 0; i < kTotalSamples; ++i) {
        double acc = 0.0;
        const int refStart = kPreRoll + i - kDelaySamples;
        for (int k = 0; k < kIrLen; ++k) {
            acc += double(ref[size_t(refStart - k)]) * double(ir[size_t(k)]);
        }
        micEcho[size_t(i)] = float(acc);
    }

    // Run AEC across 3.5 seconds of echo to allow filter adaptation
    std::vector<float> cleanOut(kFrameSize);
    double echoInPower = 0.0, echoOutPower = 0.0;

    for (int f = 0; f < 350; ++f) {
        const float* refFrame = ref.data() + kPreRoll + f * kFrameSize;
        const float* micFrame = micEcho.data() + f * kFrameSize;

        aec.pushReference(refFrame, kFrameSize);
        aec.process(micFrame, cleanOut.data(), kFrameSize, kDelaySamples);

        if (f % 50 == 0 || f == 349) {
            float pIn = 0.0f, pOut = 0.0f;
            for (int i = 0; i < kFrameSize; ++i) {
                pIn += micFrame[i] * micFrame[i];
                pOut += cleanOut[size_t(i)] * cleanOut[size_t(i)];
            }
            std::printf("    frame %3d: pIn=%.4f pOut=%.4f (ratio=%.2f dB) erleDb=%.1f\n",
                        f, pIn, pOut, 10.0f * std::log10((pIn + 1e-9f) / (pOut + 1e-9f)), aec.erleDb());
        }

        // Measure ERLE on frames 250..350 (after 2.5s to 3.5s of adaptation)
        if (f >= 250) {
            for (int i = 0; i < kFrameSize; ++i) {
                echoInPower += double(micFrame[i]) * double(micFrame[i]);
                echoOutPower += double(cleanOut[size_t(i)]) * double(cleanOut[size_t(i)]);
            }
        }
    }

    REQUIRE(echoInPower > 0.0);
    REQUIRE(echoOutPower > 0.0);
    const double erle = 10.0 * std::log10(echoInPower / echoOutPower);
    std::printf("  FriendEcho AEC ERLE after 3.0 s: %.1f dB (target >= 25.0 dB)\n", erle);
    CHECK(erle >= 25.0);

    // Measure the exact filter delay of FriendEcho by pulsing an impulse through an instance
    int filterDelaySamples = 0;
    {
        ssnet::FriendEcho probeAec;
        probeAec.init(kSampleRate, kFrameSize, 250);
        probeAec.setMode(ssnet::AecMode::On);
        std::vector<float> impulseIn(kFrameSize, 0.0f), probeOut(kFrameSize, 0.0f);
        impulseIn[0] = 0.8f;
        std::vector<float> probeHistory;
        for (int p = 0; p < 4; ++p) {
            probeAec.pushReference(probeOut.data(), kFrameSize);
            probeAec.process(p == 0 ? impulseIn.data() : probeOut.data(), probeOut.data(), kFrameSize, 0);
            probeHistory.insert(probeHistory.end(), probeOut.begin(), probeOut.end());
        }
        float maxVal = 0.0f;
        for (int i = 0; i < int(probeHistory.size()); ++i) {
            if (std::abs(probeHistory[size_t(i)]) > maxVal) {
                maxVal = std::abs(probeHistory[size_t(i)]);
                filterDelaySamples = i;
            }
        }
        std::printf("  FriendEcho impulse response peak: sample %d (%.1f ms), maxAmp=%.4f\n",
                    filterDelaySamples, 1000.0f * float(filterDelaySamples) / float(kSampleRate), maxVal);
    }

    // Now test Double-Talk: near-end vocal singing (with vibrato) superimposed on echo (frames 350..450)
    std::vector<float> allNear, allClean, allPureSpeechClean;
    for (int f = 350; f < 450; ++f) {
        const float* refFrame = ref.data() + kPreRoll + f * kFrameSize;
        std::vector<float> dtMic(kFrameSize), nearSpeech(kFrameSize);

        for (int i = 0; i < kFrameSize; ++i) {
            const float t = float(f * kFrameSize + i) / float(kSampleRate);
            // Singing voice with natural 5.5 Hz vibrato (modulating harmonics 260 Hz, 520 Hz, 780 Hz)
            const float vib = 1.0f + 0.03f * std::sin(2.0f * 3.14159265f * 5.5f * t);
            nearSpeech[size_t(i)] = 0.30f * std::sin(2.0f * 3.14159265f * 260.0f * vib * t) +
                                    0.18f * std::sin(2.0f * 3.14159265f * 520.0f * vib * t) +
                                    0.08f * std::sin(2.0f * 3.14159265f * 780.0f * vib * t);
            dtMic[size_t(i)] = micEcho[size_t(f * kFrameSize + i)] + nearSpeech[size_t(i)];
        }

        aec.pushReference(refFrame, kFrameSize);
        aec.process(dtMic.data(), cleanOut.data(), kFrameSize, kDelaySamples);

        allNear.insert(allNear.end(), nearSpeech.begin(), nearSpeech.end());
        allClean.insert(allClean.end(), cleanOut.begin(), cleanOut.end());
    }

    // Also pass nearSpeech through a baseline FriendEcho without any echo to see pure filter effect
    {
        ssnet::FriendEcho pureAec;
        pureAec.init(kSampleRate, kFrameSize, 200);
        pureAec.setMode(ssnet::AecMode::On);
        std::vector<float> dummyRef(kFrameSize, 0.0f), pOut(kFrameSize);
        for (int f = 350; f < 450; ++f) {
            const float* s = allNear.data() + (f - 350) * kFrameSize;
            pureAec.pushReference(dummyRef.data(), kFrameSize);
            pureAec.process(s, pOut.data(), kFrameSize, 0);
            allPureSpeechClean.insert(allPureSpeechClean.end(), pOut.begin(), pOut.end());
        }
    }

    // Search best lag within [0, 2 * kFrameSize]
    int bestLag = 0;
    double maxCorr = -1e9;
    const int maxLag = kFrameSize * 2;
    const int evalStart = kFrameSize * 30; // skip initial frames of DT
    const int evalLen = int(allNear.size()) - evalStart - maxLag - 1;

    for (int lag = 0; lag <= maxLag; ++lag) {
        double corr = 0.0;
        for (int i = 0; i < evalLen; ++i) {
            corr += double(allClean[size_t(evalStart + lag + i)]) * double(allNear[size_t(evalStart + i)]);
        }
        if (corr > maxCorr) {
            maxCorr = corr;
            bestLag = lag;
        }
    }



    // Measure speech preservation during double-talk:
    // Speex preprocess applies IIR DC-notch, pre-emphasis, and WOLA analysis/synthesis filters.
    // The distortion introduced by the presence of echo during double talk is (allClean - allPureSpeechClean).
    double speechPower = 0.0, distPower = 0.0;
    for (int i = 0; i < evalLen; ++i) {
        const double s = allPureSpeechClean[size_t(evalStart + bestLag + i)];
        const double o = allClean[size_t(evalStart + bestLag + i)];
        const double diff = o - s;
        speechPower += s * s;
        distPower += diff * diff;
    }

    REQUIRE(distPower > 0.0);
    const double speechSnr = 10.0 * std::log10(speechPower / distPower);
    std::printf("  FriendEcho AEC double-talk speech SNR: %.1f dB (target >= 15.0 dB)\n", speechSnr);
    CHECK(speechSnr >= 15.0);
}

