// Writes compat/golden/net (PLAN-rust-migration R3.1): Opus packets and decoded audio, and FriendEcho output, from
// the C++ ssnet, so hs-net can check it makes the same bytes. Inputs are written too (the Rust side reads them).
//   ss_net_golden <out-dir>
// SPDX-License-Identifier: MIT
#include "ssnet/aec.h"
#include "ssnet/opus_codec.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ssnet;

namespace {

std::string dir;

void save(const std::string& name, const void* data, size_t bytes) {
    const std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr || std::fwrite(data, 1, bytes, f) != bytes) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fclose(f);
}

void saveF32(const std::string& name, const std::vector<float>& v) { save(name, v.data(), v.size() * sizeof(float)); }

uint32_t lcg = 1;
float noise() {
    lcg = lcg * 1664525u + 1013904223u;
    return float(int(lcg >> 16) - 32768) / 32768.0f;
}

// packets as (u16 little-endian length, bytes) one after another
void appendPacket(std::vector<uint8_t>& out, const uint8_t* p, int n) {
    out.push_back(uint8_t(n & 0xff));
    out.push_back(uint8_t(n >> 8));
    out.insert(out.end(), p, p + n);
}

// Encodes `in` (planar, `chans` channels of `frames` each) in packets of `pkt` frames, then decodes them: every
// 10th packet is lost (PLC), and packet 25 is recovered from the FEC in packet 26.
void opusCase(const std::string& name, OpusEnc::App app, int chans, int bitrate, int pkt,
              const std::vector<std::vector<float>>& in) {
    OpusEnc enc;
    OpusDec dec;
    if (!enc.init(48000, chans, app) || !dec.init(48000, chans)) std::exit(2);
    if (bitrate > 0) enc.setBitrate(bitrate);
    const int count = int(in[0].size()) / pkt;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> all;
    uint8_t buf[1500];
    for (int p = 0; p < count; ++p) {
        const float* ch[2] = { in[0].data() + p * pkt, in[size_t(chans - 1)].data() + p * pkt };
        const int n = enc.encode(ch, pkt, buf, int(sizeof(buf)));
        if (n <= 0) std::exit(3);
        packets.emplace_back(buf, buf + n);
        appendPacket(all, buf, n);
    }
    save(name + ".pkt", all.data(), all.size());

    std::vector<float> out;
    std::vector<float> l(static_cast<size_t>(pkt)), r(static_cast<size_t>(pkt));
    for (int p = 0; p < count; ++p) {
        float* ch[2] = { l.data(), r.data() };
        int got;
        if (p % 10 == 9) got = dec.decodePlc(ch, pkt);
        else if (p == 25) got = dec.decode(packets[26].data(), int(packets[26].size()), ch, pkt, true);
        else got = dec.decode(packets[size_t(p)].data(), int(packets[size_t(p)].size()), ch, pkt);
        if (got != pkt) std::exit(4);
        for (int i = 0; i < pkt; ++i) {
            out.push_back(l[size_t(i)]);
            if (chans == 2) out.push_back(r[size_t(i)]);
        }
    }
    saveF32(name + "_dec.f32", out);
}

void aecCase() {
    constexpr int kFrame = 480, kFrames = 300, kDelay = 960;
    const int total = kFrame * kFrames;
    std::vector<float> ref(static_cast<size_t>(total + kDelay)), mic(static_cast<size_t>(total));
    lcg = 7;
    for (float& v : ref) v = 0.4f * noise();
    for (int i = 0; i < total; ++i) {
        const float nearEnd = i >= total / 2 ? 0.2f * std::sin(2.0f * 3.14159265f * 260.0f * float(i) / 48000.0f) : 0.0f;
        mic[size_t(i)] = 0.3f * ref[size_t(i)] - 0.1f * ref[size_t(i + 40)] + nearEnd;   // ref[i] is played kDelay early
    }
    saveF32("aec_ref.f32", ref);
    saveF32("aec_mic.f32", mic);

    FriendEcho aec;
    if (!aec.init(48000, kFrame, 200)) std::exit(5);
    aec.setMode(AecMode::On);
    std::vector<float> out(static_cast<size_t>(total)), erle;
    for (int f = 0; f < kFrames; ++f) {
        aec.pushReference(ref.data() + kDelay + f * kFrame, kFrame);
        aec.process(mic.data() + f * kFrame, out.data() + f * kFrame, kFrame, kDelay - 40);
        erle.push_back(aec.erleDb());
    }
    saveF32("aec_out.f32", out);
    saveF32("aec_erle.f32", erle);
}

// As ShareServer uses it: 240-frame steps processed in place. Loud enough to clip, the delay moves at frame 150
// and the canceller is reset at frame 300.
void aec240Case() {
    constexpr int kFrame = 240, kFrames = 400;
    const int total = kFrame * kFrames;
    std::vector<float> ref(static_cast<size_t>(total + 2000)), mic(static_cast<size_t>(total));
    lcg = 11;
    for (float& v : ref) v = 1.3f * noise();
    for (int i = 0; i < total; ++i) mic[size_t(i)] = 0.9f * ref[size_t(i + 700)] + 0.3f * ref[size_t(i + 650)];
    saveF32("aec240_ref.f32", ref);
    saveF32("aec240_mic.f32", mic);

    FriendEcho aec;
    if (!aec.init(48000, kFrame, 200)) std::exit(6);
    aec.setMode(AecMode::On);
    std::vector<float> out(mic), erle;
    for (int f = 0; f < kFrames; ++f) {
        if (f == 300) aec.reset();
        aec.pushReference(ref.data() + 2000 + f * kFrame, kFrame);
        float* io = out.data() + f * kFrame;
        aec.process(io, io, kFrame, f < 150 ? 1300 : 1700);
        erle.push_back(aec.erleDb());
    }
    saveF32("aec240_out.f32", out);
    saveF32("aec240_erle.f32", erle);
}

// Interleaved stereo VOIP: encodeInterleaved / decodeInterleaved, every 10th packet concealed.
void interleavedCase(const std::vector<float>& stereo) {
    OpusEnc enc;
    OpusDec dec;
    if (!enc.init(48000, 2, OpusEnc::App::Voip) || !dec.init(48000, 2)) std::exit(7);
    enc.setBitrate(96000);
    std::vector<uint8_t> all;
    std::vector<float> out, pcm(960);
    uint8_t buf[1500];
    for (int p = 0; p < int(stereo.size()) / 960; ++p) {
        const int n = enc.encodeInterleaved(stereo.data() + p * 960, 480, buf, int(sizeof(buf)));
        if (n <= 0) std::exit(8);
        appendPacket(all, buf, n);
        const int got = p % 10 == 9 ? dec.decodePlcInterleaved(pcm.data(), 480)
                                    : dec.decodeInterleaved(buf, n, pcm.data(), 480);
        if (got != 480) std::exit(9);
        out.insert(out.end(), pcm.begin(), pcm.end());
    }
    save("opus_interleaved.pkt", all.data(), all.size());
    saveF32("opus_interleaved_dec.f32", out);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ss_net_golden <out-dir>\n");
        return 1;
    }
    dir = argv[1];

    constexpr int kLen = 48000;   // 1 s
    std::vector<float> mono(kLen), left(kLen), right(kLen);
    lcg = 1;
    for (int i = 0; i < kLen; ++i) {
        const float t = float(i) / 48000.0f;
        mono[size_t(i)] = 0.4f * std::sin(2.0f * 3.14159265f * 440.0f * t) + 0.05f * noise();
        left[size_t(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 300.0f * t);
        right[size_t(i)] = 0.3f * std::sin(2.0f * 3.14159265f * 650.0f * t) + 0.02f * noise();
    }
    saveF32("input_mono.f32", mono);
    std::vector<float> stereo;
    for (int i = 0; i < kLen; ++i) {
        stereo.push_back(left[size_t(i)]);
        stereo.push_back(right[size_t(i)]);
    }
    saveF32("input_stereo.f32", stereo);

    opusCase("opus_voip", OpusEnc::App::Voip, 1, 64000, 480, { mono });
    opusCase("opus_audio_stereo", OpusEnc::App::Audio, 2, 128000, 960, { left, right });
    opusCase("opus_lowdelay", OpusEnc::App::LowDelay, 1, 0, 480, { mono });
    interleavedCase(stereo);
    aecCase();
    aec240Case();
    std::printf("net goldens written to %s\n", dir.c_str());
    return 0;
}
