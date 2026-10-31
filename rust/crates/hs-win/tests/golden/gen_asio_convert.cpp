// Writes asio_convert.golden: what the C++ HasAsio.cpp (apps/karaoke) makes of the same inputs the Rust test builds.
// The conversion code is not copied: run_golden.ps1 compiles HasAsio.cpp as it is, with a stub HasAsio.h in front
// of the JUCE one. Inputs: the same LCG and lists as tests/convert.rs (keep the two in step).
#include "HasAsio.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace hearaside::karaoke::asio;

static uint64_t gState = 0x1234567887654321ull;
static uint32_t nextU32() {
    gState = gState * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(gState >> 32);
}
static double nextUnit() { return double(nextU32()) / 4294967296.0; }   // [0, 1)

static std::vector<float> floatInputs() {
    std::vector<float> f = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.999f, 1.5f, -1.5f, 2.0f, 1.0e-9f, -1.0e-9f,
                             0.9999999f, -0.9999999f, 3.4e38f, -3.4e38f, INFINITY, -INFINITY };
    for (int bits : { 16, 18, 20, 24 }) {   // values on and next to the half-way points of each integer type
        const double fs = double(1 << (bits - 1));
        for (int k : { 0, 1, 2, 3, -1, -2, 100, -100, 12345, -12345 })
            for (double d : { -0.5, 0.0, 0.5, 0.25, -0.25, 0.4999, 0.5001 })
                f.push_back(float((double(k) + d) / (fs - 1.0)));
    }
    for (int i = 0; i < 1024; ++i) f.push_back(float(nextUnit() * 2.4 - 1.2));
    return f;
}

static constexpr int kRaw = 512;

static std::vector<unsigned char> rawInputs(long type) {
    const int bytes = bytesPerSample(type);
    std::vector<unsigned char> r(size_t(bytes) * kRaw);
    if (type == Float32LSB) {
        for (int i = 0; i < kRaw; ++i) {
            const float v = float((nextUnit() * 2.0 - 1.0) * (i % 7 == 0 ? 3.0e30 : 1.3));
            std::memcpy(&r[size_t(4 * i)], &v, 4);
        }
    } else if (type == Float64LSB) {
        for (int i = 0; i < kRaw; ++i) {
            const double v = (nextUnit() * 2.0 - 1.0) * (i % 7 == 0 ? 1.0e300 : 1.3) * (i % 11 == 0 ? 1.0e-3 : 1.0);
            std::memcpy(&r[size_t(8 * i)], &v, 8);
        }
    } else {
        for (size_t i = 0; i < r.size(); ++i) r[i] = (unsigned char)(nextU32() >> 24);
    }
    return r;
}

static void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }

int main(int, char** argv) {
    FILE* out = std::fopen(argv[1], "wb");
    if (!out) return 1;
    const std::vector<float> in = floatInputs();
    for (long type : { 16L, 17L, 18L, 19L, 20L, 24L, 25L, 26L, 27L }) {
        const int bytes = bytesPerSample(type);
        std::vector<unsigned char> enc(size_t(bytes) * in.size());
        if (!fromFloat(type, in.data(), enc.data(), int(in.size()))) return 2;
        put32(out, uint32_t(enc.size()));
        std::fwrite(enc.data(), 1, enc.size(), out);
        const std::vector<unsigned char> raw = rawInputs(type);
        std::vector<float> dec(kRaw);
        if (!toFloat(type, raw.data(), dec.data(), kRaw)) return 3;
        put32(out, uint32_t(dec.size()));
        for (float v : dec) {
            uint32_t b;
            std::memcpy(&b, &v, 4);
            put32(out, b);
        }
    }
    std::fclose(out);
    return 0;
}
