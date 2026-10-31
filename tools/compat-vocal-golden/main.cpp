// SPDX-License-Identifier: MIT
#include "VocalChain.h"
#include "rt_audit.h"

#include <juce_audio_formats/juce_audio_formats.h> // --render (already linked through juce_dsp)

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace {
constexpr int sampleRate = 48000;
constexpr int blockSize = 256;
constexpr int frames = 32768;
constexpr int activeFrames = 8192;
constexpr uint32_t inputSeed = 0x564f4341;
constexpr double twoPi = 6.283185307179586476925286766559;

void require(bool condition, const juce::String& message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

juce::MemoryBlock floatBytes(const std::vector<float>& values) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    juce::MemoryBlock bytes(values.size() * 4);
    auto* out = static_cast<uint8_t*>(bytes.getData());
    for (size_t i = 0; i < values.size(); ++i) {
        require(std::isfinite(values[i]), "Nonfinite golden sample");
        const uint32_t bits = std::bit_cast<uint32_t>(values[i]);
        for (int byte = 0; byte < 4; ++byte) out[i * 4 + size_t(byte)] = uint8_t(bits >> (byte * 8));
    }
    return bytes;
}

std::vector<float> makeInput(int stimulus) {
    std::vector<float> input(frames, 0.0f);
    uint32_t random = inputSeed;
    for (int i = 0; i < activeFrames; ++i) {
        random = random * 1664525u + 1013904223u;
        const double noise = double(random >> 8) / 8388608.0 - 1.0;
        const double time = double(i) / sampleRate;
        switch (stimulus) {
            case 0: input[size_t(i)] = float(0.65 * std::sin(twoPi * 997.0 * time)); break;
            case 1: input[size_t(i)] = float(noise * (i < 2048 || i >= 6144 ? 0.0005 : 0.45)); break;
            case 2: input[size_t(i)] = i == 1024 ? 0.8f : i == 4096 ? -0.4f : 0.0f; break;
            case 3: {
                const double envelope = 1.0 - double(i % 2048) / 2048.0;
                const double chord = std::sin(twoPi * 220.0 * time)
                    + 0.6 * std::sin(twoPi * 277.1826309768721 * time)
                    + 0.4 * std::sin(twoPi * 329.6275569128699 * time);
                input[size_t(i)] = float(envelope * (0.3 * chord + 0.05 * noise));
                break;
            }
        }
    }
    return input;
}

struct Settings {
    const char* name;
    float reverb, echo, gateDb;
    bool sendWet, muted;
};

bool sameMetadata(const juce::var& actual, const juce::var& expected) {
    if (auto* object = actual.getDynamicObject()) {
        auto* other = expected.getDynamicObject();
        if (!other || object->getProperties().size() != other->getProperties().size()) return false;
        for (const auto& property : object->getProperties()) {
            if (!other->hasProperty(property.name)) return false;
            // Audio samples are compared directly; these measurements can change with compiler rounding.
            if (property.name == juce::Identifier("peak") || property.name == juce::Identifier("tailPeak")
                || property.name == juce::Identifier("stereoDifference")) continue;
            if (!sameMetadata(property.value, other->getProperty(property.name))) return false;
        }
        return true;
    }
    if (auto* array = actual.getArray()) {
        auto* other = expected.getArray();
        if (!other || array->size() != other->size()) return false;
        for (int i = 0; i < array->size(); ++i) if (!sameMetadata((*array)[i], (*other)[i])) return false;
        return true;
    }
    if (actual.isString()) return expected.isString() && actual.toString() == expected.toString();
    if (actual.isBool()) return expected.isBool() && actual == expected;
    const auto numeric = [](const juce::var& value) { return value.isInt() || value.isInt64() || value.isDouble(); };
    if (numeric(actual)) return numeric(expected) && double(actual) == double(expected);
    return actual.isVoid() && expected.isVoid();
}

void run(const juce::File& directory, bool check, bool compare) {
    if (check) require(directory.isDirectory(), "Golden directory does not exist");
    else {
        require(!directory.exists() || (directory.isDirectory()
            && directory.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty()),
            "Output directory must be empty");
        require(directory.createDirectory().wasOk(), "Cannot create output directory");
    }
    double maximumError = 0;
    auto store = [&](const juce::String& name, const juce::MemoryBlock& data) {
        const auto file = directory.getChildFile(name);
        if (check) {
            juce::MemoryBlock expected;
            require(file.loadFileAsData(expected), "Cannot read golden file: " + name);
            if (compare && name.endsWith(".f32")) {
                require(expected.getSize() == data.getSize(), "Golden sample count mismatch: " + name);
                auto sample = [](const juce::MemoryBlock& block, size_t offset) {
                    const auto* bytes = static_cast<const uint8_t*>(block.getData()) + offset;
                    const auto bits = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8)
                        | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
                    return std::bit_cast<float>(bits);
                };
                double fileError = 0;
                for (size_t offset = 0; offset < data.getSize(); offset += 4) {
                    const auto before = sample(expected, offset), after = sample(data, offset);
                    require(std::isfinite(before) && std::isfinite(after), "Nonfinite golden sample: " + name);
                    fileError = std::max(fileError, std::abs(double(before) - double(after)));
                }
                require(fileError <= 1.0e-6, "Golden error exceeds 1e-6: " + name);
                maximumError = std::max(maximumError, fileError);
            } else if (compare && name == "manifest.json") {
                juce::var before, after;
                require(juce::JSON::parse(expected.toString(), before).wasOk()
                    && juce::JSON::parse(data.toString(), after).wasOk(), "Invalid golden manifest JSON");
                require(sameMetadata(after, before), "Golden metadata mismatch");
            } else require(expected == data, "Golden mismatch: " + name);
        } else require(file.replaceWithData(data.getData(), data.getSize()), "Cannot write golden file: " + name);
    };
    const char* stimuli[] = { "sine", "noise", "impulse", "synthetic-music" };
    const Settings settings[] = {
        { "default-dry-send", 0.25f, 0.20f, -45.0f, false, false },
        { "ambience-off", 0.0f, 0.0f, -45.0f, false, false },
        { "wet-send", 0.65f, 0.55f, -36.0f, true, false },
        { "gate-closed", 0.0f, 0.0f, 6.0f, false, false },
        { "muted", 0.65f, 0.55f, -36.0f, true, true }
    };
    auto* manifest = new juce::DynamicObject();
    const juce::var manifestValue(manifest);
    manifest->setProperty("schemaVersion", 1);
    manifest->setProperty("source", "apps/karaoke/VocalChain.cpp");
    manifest->setProperty("sourceCommit", "7f4eccc298734d78667b67fa51ee81dee41d0e2b");
    manifest->setProperty("encoding", "IEEE754-f32-little-endian");
    manifest->setProperty("sampleRate", sampleRate);
    manifest->setProperty("blockSize", blockSize);
    manifest->setProperty("frames", frames);
    manifest->setProperty("activeInputFrames", activeFrames);
    manifest->setProperty("tailFrames", frames - activeFrames);
    manifest->setProperty("inputSeed", juce::int64(inputSeed));
    manifest->setProperty("inputGenerator", "LCG1664525/1013904223; original sine/noise/impulse/synthetic chord; source in main.cpp");
    manifest->setProperty("outputChannels", "interleaved headphone-left, headphone-right, network-mono");
    manifest->setProperty("bypassReference", "input-*.f32 is unprocessed mono input; VocalChain has no bypass API");
    manifest->setProperty("fixedProcessing", "80Hz highpass,2.5kHz peak,10kHz shelf; compressor3:1/-16dBFS/+2dB makeup; unchanged production source");
    manifest->setProperty("comparison", "bitwise regeneration on reference toolchain; Rust DSP migration target max absolute sample error1e-6(-120dBFS)");
    juce::Array<juce::var> cases;
    int fileCount = 0;
    for (int stimulus = 0; stimulus < 4; ++stimulus) {
        const auto input = makeInput(stimulus);
        const auto inputFile = juce::String("input-") + stimuli[stimulus] + ".f32";
        store(inputFile, floatBytes(input));
        ++fileCount;
        for (const auto& setting : settings) {
            hearaside::karaoke::VocalChain chain;
            chain.prepare(sampleRate, blockSize);
            if (juce::String(setting.name) == "default-dry-send") {
                require(chain.reverbLevel() == setting.reverb && chain.echoLevel() == setting.echo
                    && !chain.isSendWet() && !chain.isMuted(), "Production VocalChain defaults changed");
            } else {
                chain.setReverbLevel(setting.reverb);
                chain.setEchoLevel(setting.echo);
                chain.setGateThresholdDb(setting.gateDb);
                chain.setSendWet(setting.sendWet);
                chain.setMuted(setting.muted);
            }
            std::vector<float> output(size_t(frames) * 3);
            float left[blockSize], right[blockSize], network[blockSize];
            float peak = 0, tailPeak = 0, stereoDifference = 0;
            for (int offset = 0; offset < frames; offset += blockSize) {
                {
                    rt::Scope audio("VocalChain::process");
                    chain.process(input.data() + offset, left, right, network, blockSize);
                }
                for (int i = 0; i < blockSize; ++i) {
                    const auto index = size_t(offset + i) * 3;
                    output[index] = left[i]; output[index + 1] = right[i]; output[index + 2] = network[i];
                    peak = std::max(peak, std::max(std::abs(left[i]), std::abs(right[i])));
                    stereoDifference = std::max(stereoDifference, std::abs(left[i] - right[i]));
                    if (offset + i >= activeFrames) tailPeak = std::max(tailPeak, std::max(std::abs(left[i]), std::abs(right[i])));
                    if (setting.muted) require(left[i] == 0 && right[i] == 0 && network[i] == 0, "Muted output must be silent");
                    if (setting.reverb == 0 && setting.echo == 0)
                        require(left[i] == right[i] && left[i] == network[i], "Ambience-off routing must match dry mono send");
                    if (setting.sendWet && !setting.muted)
                        require(std::abs(network[i] - 0.5f * (left[i] + right[i])) < 1.0e-6f, "Wet send must average headphone channels");
                }
            }
            if (!setting.muted && setting.gateDb < 0) require(peak > 0.001f, "Ordinary scenario must contain audible signal");
            if (!setting.muted && setting.reverb > 0) {
                require(tailPeak > 0.00001f, "Ambience scenario must retain a nonzero tail");
                require(stereoDifference > 0.00001f, "Ambience scenario must produce different left and right channels");
            }
            if (setting.gateDb > 0) require(chain.currentGateGain() < 1.0e-6f, "Closed gate must settle after its release slew");
            const auto name = juce::String(stimuli[stimulus]) + "-" + setting.name + ".f32";
            store(name, floatBytes(output));
            ++fileCount;
            auto* entry = new juce::DynamicObject();
            entry->setProperty("stimulus", stimuli[stimulus]);
            entry->setProperty("scenario", setting.name);
            entry->setProperty("inputFile", inputFile);
            entry->setProperty("outputFile", name);
            entry->setProperty("channels", 3);
            entry->setProperty("reverbLevel", setting.reverb);
            entry->setProperty("echoLevel", setting.echo);
            entry->setProperty("gateThresholdDb", setting.gateDb);
            entry->setProperty("sendWet", setting.sendWet);
            entry->setProperty("muted", setting.muted);
            entry->setProperty("peak", peak);
            entry->setProperty("tailPeak", tailPeak);
            entry->setProperty("stereoDifference", stereoDifference);
            cases.add(juce::var(entry));
        }
    }
    manifest->setProperty("cases", juce::var(cases));
    const auto json = juce::JSON::toString(manifestValue, false).replace("\r\n", "\n") + "\n";
    store("manifest.json", juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()));
    require(directory.findChildFiles(juce::File::findFilesAndDirectories, false).size() == fileCount + 1,
        "Unexpected files in golden directory");
    std::printf("%s20 VocalChain cases and 4 input references\n", check ? "Checked " : "Wrote ");
    if (compare) std::printf("Maximum absolute sample error: %.9g (limit 1e-6)\n", maximumError);
}
// A/B listening (R5.3): runs a recording's first channel through VocalChain with the app's default settings and
// writes the headphone mix as 32-bit float stereo WAV. `cargo run -p hs-karaoke --example vocal_render` is the Rust twin.
int render(const juce::File& in, const juce::File& out) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(in));
    require(reader != nullptr, "Cannot read " + in.getFullPathName());
    const int length = int(reader->lengthInSamples);
    juce::AudioBuffer<float> mic(int(reader->numChannels), length);
    reader->read(&mic, 0, length, 0, true, true);

    hearaside::karaoke::VocalChain chain;
    chain.prepare(reader->sampleRate, blockSize);
    juce::AudioBuffer<float> mix(2, length);
    std::vector<float> network(blockSize);
    for (int offset = 0; offset < length; offset += blockSize) {
        const int n = std::min(blockSize, length - offset);
        chain.process(mic.getReadPointer(0, offset), mix.getWritePointer(0, offset), mix.getWritePointer(1, offset), network.data(), n);
    }

    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = out.createOutputStream();
    require(stream != nullptr, "Cannot write " + out.getFullPathName());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(reader->sampleRate).withNumChannels(2).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    require(writer != nullptr && writer->writeFromAudioSampleBuffer(mix, 0, length), "Cannot write " + out.getFullPathName());
    std::printf("Rendered %d frames at %.0f Hz\n", length, reader->sampleRate);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && juce::String(argv[1]) == "--render") {
        try {
            const auto cwd = juce::File::getCurrentWorkingDirectory();
            return render(cwd.getChildFile(argv[2]), cwd.getChildFile(argv[3]));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    const juce::String mode = argc == 3 ? argv[1] : "";
    if (mode != "--output" && mode != "--check" && mode != "--compare" && mode != "--rt-audit") {
        std::fprintf(stderr, "Usage: hs-vocal-golden --output <empty-directory> | --check <fixture-directory> | --compare <fixture-directory> | --rt-audit <fixture-directory>\n");
        return 2;
    }
    try {
        const bool audit = mode == "--rt-audit";
        if (audit) {
            std::mutex probe;
            require(rt::selfTest(probe), "Real-time audit detectors did not fire");
            rt::reset();
        }
        run(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]), mode != "--output", mode == "--compare" || audit);
        if (audit) {
            const auto* site = rt::site("VocalChain::process");
            const auto allocations = site->allocations.load(), locks = site->locks.load();
            std::printf("VocalChain::process: %llu allocations, %s\n", (unsigned long long) allocations,
                rt::kDetectsLocks ? (juce::String((juce::int64) locks) + " blocking locks").toRawUTF8() : "locks not detected here");
            require(allocations == 0 && locks == 0, "VocalChain::process allocated or blocked on the audio thread");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
