#include "testing.h"

#include "AudioSettings.h"
#include "JoinClient.h"
#include "PreflightChecker.h"
#include "VocalChain.h"
#include "MusicPlayer.h"
#include "RoomHost.h"
#include "UpdateChecker.h"
#include "ShareServer.h"
#include "ssbus/bus.h"
#include "ssnet/wire.h"
#include "ssnet/opus_codec.h"
#include "ssnet/aec.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace hearaside;
using namespace hearaside::karaoke;

TEST_CASE("karaoke: VocalChain gate, EQ, compressor, reverb, echo, and routing") {
    VocalChain chain;
    chain.prepare(48000.0, 256);

    std::vector<float> inMic(256, 0.0f);
    std::vector<float> outL(256, 0.0f);
    std::vector<float> outR(256, 0.0f);
    std::vector<float> outNet(256, 0.0f);

    // 1. Quiet noise below gate threshold (-45 dBFS, ~0.0056)
    for (int i = 0; i < 256; ++i) inMic[i] = 0.001f;
    for (int b = 0; b < 20; ++b) {
        chain.process(inMic.data(), outL.data(), outR.data(), outNet.data(), 256);
    }
    float quietPeak = 0.0f;
    for (int i = 0; i < 256; ++i) quietPeak = std::max(quietPeak, std::abs(outNet[i]));
    CHECK(quietPeak < 0.0005f); // Gate attenuated below threshold

    // 2. Vocal signal above threshold (1 kHz sine at -10 dBFS, ~0.316)
    for (int i = 0; i < 256; ++i) {
        inMic[i] = 0.316f * std::sin(2.0f * 3.14159265f * 1000.0f * float(i) / 48000.0f);
    }
    for (int b = 0; b < 10; ++b) {
        chain.process(inMic.data(), outL.data(), outR.data(), outNet.data(), 256);
    }
    float vocalPeak = 0.0f;
    for (int i = 0; i < 256; ++i) vocalPeak = std::max(vocalPeak, std::abs(outNet[i]));
    CHECK(vocalPeak > 0.1f); // Gate opened and vocal passed through cleanly

    // 3. Mute check
    chain.setMuted(true);
    chain.process(inMic.data(), outL.data(), outR.data(), outNet.data(), 256);
    float mutedPeak = 0.0f;
    for (int i = 0; i < 256; ++i) {
        mutedPeak = std::max(mutedPeak, std::max(std::abs(outL[i]), std::abs(outNet[i])));
    }
    CHECK(mutedPeak == 0.0f); // strictly silent when muted

    // 4. Dry vs Wet network send routing
    chain.setMuted(false);
    chain.setSendWet(false);
    chain.setReverbLevel(0.8f);
    chain.setEchoLevel(0.8f);

    // Let reverb build up with vocal
    for (int b = 0; b < 10; ++b) {
        chain.process(inMic.data(), outL.data(), outR.data(), outNet.data(), 256);
    }
    // Now silence input: with sendWet == false, outNet should quickly drop towards 0, while outL/outR ring out with reverb
    std::fill(inMic.begin(), inMic.end(), 0.0f);
    for (int b = 0; b < 10; ++b) {
        chain.process(inMic.data(), outL.data(), outR.data(), outNet.data(), 256);
    }
    float dryNetPeak = 0.0f;
    float wetHeadphonePeak = 0.0f;
    for (int i = 0; i < 256; ++i) {
        dryNetPeak = std::max(dryNetPeak, std::abs(outNet[i]));
        wetHeadphonePeak = std::max(wetHeadphonePeak, std::abs(outL[i]));
    }
    CHECK(dryNetPeak < 0.001f); // dry send drops with dry input
    CHECK(wetHeadphonePeak > 0.01f); // headphones still ring with reverb tail!
}

TEST_CASE("karaoke: PreflightChecker acoustic leak detector (closed headphones, open headphones, speakers)") {
    constexpr double kSampleRate = 48000.0;
    constexpr int kChirpLen = 2400; // 50 ms
    constexpr int kTotalLen = 12000; // 250 ms

    // Synthesize test chirp reference
    std::vector<float> chirpRef(kChirpLen);
    const double f0 = 500.0, f1 = 2500.0, T = double(kChirpLen) / kSampleRate;
    const double kSweep = (f1 - f0) / T;
    const double twoPi = 2.0 * 3.14159265358979323846;
    for (int i = 0; i < kChirpLen; ++i) {
        const double t = double(i) / kSampleRate;
        const double phase = twoPi * (f0 * t + 0.5 * kSweep * t * t);
        const double win = 0.5 * (1.0 - std::cos(twoPi * double(i) / double(kChirpLen)));
        chirpRef[size_t(i)] = float(0.125 * win * std::sin(phase));
    }

    // 1. Scenario 1: Closed Headphones (clean)
    // Mic picks up low ambient noise / clean room voice only, zero chirp leak
    {
        std::vector<float> micClosed(kTotalLen, 0.0f);
        uint32_t seed = 12345;
        for (int i = 0; i < kTotalLen; ++i) {
            seed = seed * 1664525u + 1013904223u;
            // -55 dB ambient noise
            micClosed[size_t(i)] = 0.0018f * (float(int(seed >> 16) - 32768) / 32768.0f);
        }

        const auto res = PreflightChecker::evaluate(micClosed.data(), kTotalLen,
                                                    chirpRef.data(), kChirpLen,
                                                    kSampleRate, 35.0f, 4.0f, false);
        CHECK(res.leakStatus == LeakStatus::HeadphonesClean);
        CHECK(res.correlationPeak < 0.08f);
        CHECK(res.isReady);
        CHECK(res.summary == juce::String::fromUTF8("พร้อมร้อง ✓"));
    }

    // 2. Scenario 2: Open-back Headphones (slight leak, safe)
    // Chirp leaks at -42 dB relative to reference (slight bleed from earcup)
    {
        std::vector<float> micOpen(kTotalLen, 0.0f);
        constexpr int kDelay = 480; // 10 ms delay
        for (int i = 0; i < kChirpLen; ++i) {
            // Bleed attenuated by 0.008x (~ -42 dB)
            micOpen[size_t(kDelay + i)] = chirpRef[size_t(i)] * 0.008f;
        }

        const auto res = PreflightChecker::evaluate(micOpen.data(), kTotalLen,
                                                    chirpRef.data(), kChirpLen,
                                                    kSampleRate, 40.0f, 6.0f, false);
        CHECK(res.leakStatus != LeakStatus::SpeakerLeakDetected);
        CHECK(res.isReady);
        CHECK(res.summary == juce::String::fromUTF8("พร้อมร้อง ✓"));
    }

    // 3. Scenario 3: External Speakers (loud acoustic leak into microphone)
    // Speaker sound leaks into mic at -18 dB with 12 ms delay + acoustic room reflections
    {
        std::vector<float> micSpeaker(kTotalLen, 0.0f);
        constexpr int kDelay = 576; // 12 ms speaker-to-mic flight time
        for (int i = 0; i < kChirpLen; ++i) {
            micSpeaker[size_t(kDelay + i)] += chirpRef[size_t(i)] * 0.35f; // direct speaker leak
            if (kDelay + i + 144 < kTotalLen) {
                micSpeaker[size_t(kDelay + i + 144)] -= chirpRef[size_t(i)] * 0.12f; // room wall reflection 1
            }
            if (kDelay + i + 384 < kTotalLen) {
                micSpeaker[size_t(kDelay + i + 384)] += chirpRef[size_t(i)] * 0.06f; // room wall reflection 2
            }
        }

        // Test without AEC: should detect speaker leak and advise user
        const auto resNoAec = PreflightChecker::evaluate(micSpeaker.data(), kTotalLen,
                                                         chirpRef.data(), kChirpLen,
                                                         kSampleRate, 40.0f, 6.0f, false);
        CHECK(resNoAec.leakStatus == LeakStatus::SpeakerLeakDetected);
        CHECK(resNoAec.correlationPeak >= 0.20f);
        CHECK(!resNoAec.isReady);
        CHECK(resNoAec.advice.contains("AEC"));

        // Test with AEC active: system is ready because AEC is handling the speaker echo
        const auto resWithAec = PreflightChecker::evaluate(micSpeaker.data(), kTotalLen,
                                                           chirpRef.data(), kChirpLen,
                                                           kSampleRate, 40.0f, 6.0f, true);
        CHECK(resWithAec.leakStatus == LeakStatus::SpeakerLeakDetected);
        CHECK(resWithAec.isReady);
        CHECK(resWithAec.summary == juce::String::fromUTF8("พร้อมร้อง ✓"));
    }
}

TEST_CASE("karaoke: Opus HRA3 encode/decode round-trip, PLC, and framing") {
    // 1. HRA3 wire framing
    ssnet::Hra3Header hdr{};
    hdr.magic = ssnet::kMagic3;
    hdr.codec = ssnet::CodecOpus;
    hdr.chans = 1;
    hdr.flags = ssnet::FlagFec;
    hdr.seq = 100;
    hdr.pos = 960000ull;
    hdr.frames = 480;

    const uint8_t testPayload[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    uint8_t packet[sizeof(ssnet::Hra3Header) + sizeof(testPayload)];
    ssnet::writeHra3(packet, hdr, testPayload, sizeof(testPayload));

    ssnet::Hra3Header parsed{};
    const uint8_t* parsedPayload = nullptr;
    size_t parsedPayloadSize = 0;
    REQUIRE(ssnet::parseHra3(packet, sizeof(packet), parsed, parsedPayload, parsedPayloadSize));
    CHECK(parsed.magic == ssnet::kMagic3);
    CHECK(parsed.codec == ssnet::CodecOpus);
    CHECK(parsed.seq == 100);
    CHECK(parsedPayloadSize == sizeof(testPayload));
    CHECK(std::memcmp(parsedPayload, testPayload, sizeof(testPayload)) == 0);

    // 2. Opus Voip encode / decode round trip
    ssnet::OpusEnc enc;
    REQUIRE(enc.init(48000, 1, ssnet::OpusEnc::App::Voip));
    enc.setBitrate(96000); // 96 kbps
    enc.setInbandFec(true);

    ssnet::OpusDec dec;
    REQUIRE(dec.init(48000, 1));

    std::vector<float> inPcm(480);
    for (int i = 0; i < 480; ++i) {
        inPcm[size_t(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * float(i) / 48000.0f);
    }
    const float* const encCh[1] = { inPcm.data() };
    uint8_t opusBuf[1275];
    const int bytes = enc.encode(encCh, 480, opusBuf, sizeof(opusBuf));
    REQUIRE(bytes > 0);

    std::vector<float> outPcm(480, 0.0f);
    float* const decCh[1] = { outPcm.data() };
    const int decFrames = dec.decode(opusBuf, bytes, decCh, 480);
    CHECK(decFrames == 480);

    // 3. PLC on missing packet
    std::vector<float> plcPcm(480, 0.0f);
    float* const plcCh[1] = { plcPcm.data() };
    const int plcFrames = dec.decodePlc(plcCh, 480);
    CHECK(plcFrames == 480);
}

TEST_CASE("karaoke: JoinClient and ShareServer loopback in-process connection") {
#if JUCE_WINDOWS
    _putenv_s("HEARASIDE_NO_TUNNEL", "1");
#else
    setenv("HEARASIDE_NO_TUNNEL", "1", 1);
#endif
    juce::ScopedJuceInitialiser_GUI gui;

    // Initialise dummy bus on heap (BusLayout is ~20 MB, cannot be on stack)
    auto bus = std::make_unique<ssbus::BusLayout>();
    std::memset(bus.get(), 0, sizeof(ssbus::BusLayout));

    const juce::String listenToken = ShareServer::newToken();
    const juce::String sendToken = ShareServer::newToken();
    const juce::String friendToken = ShareServer::newToken();

    ShareServer server;
    server.setFriendLinks({ { 0, 101, friendToken, "Singer", "Host" } });
    REQUIRE(server.start(bus.get(), listenToken, sendToken));
    const int port = server.port();
    REQUIRE(port > 0);

    JoinClient client;
    client.prepare(48000.0, 256);

    const juce::String url = "http://127.0.0.1:" + juce::String(port) + "/s/" + friendToken;
    client.connect(url);

    // Pump and wait for connection to establish (up to 3 seconds)
    bool connected = false;
    for (int i = 0; i < 60; ++i) {
        if (juce::MessageManager::getInstanceWithoutCreating()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        if (client.isLive() || client.state() == ConnectionState::Live) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(connected);

    // Verify negotiated room metrics
    auto info = client.roomInfo();
    CHECK(info.name == "Singer");
    CHECK(info.host == "Host");
    CHECK(info.codec == "opus");

    // Wait up to 3 seconds for WebRTC DataChannel (UDP) to establish loopback
    for (int i = 0; i < 60 && !client.isUdp(); ++i) {
        if (juce::MessageManager::getInstanceWithoutCreating()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(client.isUdp());
    info = client.roomInfo();
    CHECK(info.isUdp);
    CHECK(info.transport == "WebRTC UDP");

    // Test speaker mode toggle
    client.setSpeakerMode(true);
    CHECK(client.isSpeakerMode());

    // Send audio upstream (Opus encoded via HRA3)
    std::vector<float> micChunk(480, 0.25f);
    for (int i = 0; i < 5; ++i) {
        client.pushUpstream(micChunk.data(), 480, i * 480);
        if (juce::MessageManager::getInstanceWithoutCreating()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    // Pull downstream (Opus decoded via HRA3 -> FriendReader)
    std::vector<float> downL(256, 0.0f), downR(256, 0.0f);
    client.pullDownstream(downL.data(), downR.data(), 256);

    // Disconnect
    client.disconnect();
    for (int i = 0; i < 20; ++i) {
        if (juce::MessageManager::getInstanceWithoutCreating()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
        if (!client.isLive() && client.state() == ConnectionState::Disconnected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!client.isLive());
    CHECK(client.state() == ConnectionState::Disconnected);

    server.stop();
}

TEST_CASE("karaoke: MusicPlayer file loading, playlist navigation, transport controls, and RT pullAudio") {
    const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File tempWav = tempDir.getNonexistentChildFile("test_karaoke_song", ".wav");

    // Synthesize a valid 16-bit 48kHz WAV file (0.2s 440Hz sine)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> outStream = tempWav.createOutputStream();
        CHECK(outStream != nullptr);

        auto options = juce::AudioFormatWriterOptions{}
            .withSampleRate(48000.0)
            .withNumChannels(2)
            .withBitsPerSample(16);

        auto writer = wav.createWriterFor(outStream, options);
        CHECK(writer != nullptr);

        std::vector<float> sine(9600); // 0.2s
        for (size_t i = 0; i < sine.size(); ++i) {
            sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * float(i) / 48000.0f);
        }
        const float* chs[2] = { sine.data(), sine.data() };
        writer->writeFromFloatArrays(chs, 2, 9600);
    }
    CHECK(tempWav.existsAsFile());

    MusicPlayer player;
    player.prepare(48000.0, 256);

    player.addTrack(tempWav);
    CHECK(player.trackCount() == 1);
    CHECK(player.currentTrackIndex() == 0);
    CHECK(player.lengthSeconds() > 0.15);

    // Initial state: stopped
    CHECK(!player.isPlaying());
    std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
    player.pullAudio(outL.data(), outR.data(), 256);
    float initialPeak = 0.0f;
    for (int i = 0; i < 256; ++i) initialPeak = std::max(initialPeak, std::abs(outL[i]));
    CHECK(initialPeak == 0.0f);

    // Play
    player.play();
    CHECK(player.isPlaying());

    // Pull audio blocks while playing
    float playingPeak = 0.0f;
    for (int b = 0; b < 10; ++b) {
        player.pullAudio(outL.data(), outR.data(), 256);
        for (int i = 0; i < 256; ++i) playingPeak = std::max(playingPeak, std::abs(outL[i]));
    }
    CHECK(playingPeak > 0.1f);
    CHECK(player.currentPositionSec() > 0.0);

    // Pause
    player.pause();
    CHECK(!player.isPlaying());

    // Stop and verify reset position
    player.stop();
    CHECK(!player.isPlaying());
    CHECK(player.currentPositionSec() == 0.0);

    // Volume adjustment
    player.setVolume(0.5f);
    CHECK(std::abs(player.volume() - 0.5f) < 0.01f);

    // Clear playlist and cleanup
    player.clearPlaylist();
    CHECK(player.trackCount() == 0);
    CHECK(player.currentTrackIndex() == -1);

    tempWav.deleteFile();
}

TEST_CASE("karaoke: RoomHost bus initialization, friend slot management, streamAudio for OBS, and master WAV recording") {
    RoomHost host;
    host.prepare(48000.0, 256);

    CHECK(!host.isRoomRunning());

    bool started = host.startRoom("Test Karaoke Room", 0);
    CHECK(started);
    CHECK(host.isRoomRunning());
    CHECK(host.friendCount() == 8);
    CHECK(host.serverPort() > 0);
    CHECK(host.roomName() == "Test Karaoke Room");

    // Verify friend slots
    for (int i = 0; i < 8; ++i) {
        auto s = host.slotInfo(i);
        CHECK(s.slot == i);
        CHECK(s.id == static_cast<uint32_t>(i + 1));
        CHECK(s.name == "Friend " + juce::String(i + 1));
        CHECK(!s.isMuted);
        CHECK(host.friendUrl(i).isNotEmpty());
        CHECK(host.friendDeepLink(i).startsWith("hearaside://join"));
    }

    // Mute/unmute slot
    host.setSlotMuted(3, true);
    CHECK(host.slotInfo(3).isMuted);
    host.setSlotMuted(3, false);
    CHECK(!host.slotInfo(3).isMuted);

    // Audio processing & OBS stream writing
    std::vector<float> vocL(256, 0.2f), vocR(256, 0.2f);
    std::vector<float> musL(256, 0.1f), musR(256, 0.1f);
    std::vector<float> hpL(256, 0.0f), hpR(256, 0.0f);

    host.processAudio(vocL.data(), vocR.data(), musL.data(), musR.data(), hpL.data(), hpR.data(), 256);
    CHECK(hpL[0] > 0.25f);
    CHECK(hpR[0] > 0.25f);

    // "Hear yourself" off: music only in the headphones; "Others hear you" off leaves the headphones alone
    host.processAudio(vocL.data(), vocR.data(), musL.data(), musR.data(), hpL.data(), hpR.data(), 256, false, true);
    CHECK(std::abs(hpL[0] - 0.1f) < 1e-6f);
    host.processAudio(vocL.data(), vocR.data(), musL.data(), musR.data(), hpL.data(), hpR.data(), 256, true, false);
    CHECK(std::abs(hpL[0] - 0.3f) < 1e-6f);

    // Master WAV Recording
    const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File recFile = tempDir.getNonexistentChildFile("test_host_rec", ".wav");

    bool recStarted = host.startRecording(recFile);
    CHECK(recStarted);
    CHECK(host.isRecording());

    // Process blocks while recording
    for (int b = 0; b < 20; ++b) {
        host.processAudio(vocL.data(), vocR.data(), musL.data(), musR.data(), hpL.data(), hpR.data(), 256);
    }

    host.stopRecording();
    CHECK(!host.isRecording());
    CHECK(host.recordedSeconds() > 0.0);
    CHECK(recFile.existsAsFile());

    recFile.deleteFile();

    // Stop room
    host.stopRoom();
    CHECK(!host.isRoomRunning());
}

TEST_CASE("karaoke: UpdateChecker semver comparison") {
    // Equal versions
    CHECK(UpdateChecker::compareVersions("1.0.5", "1.0.5") == 0);
    CHECK(UpdateChecker::compareVersions("1.0.0", "1.0") == 0);
    CHECK(UpdateChecker::compareVersions("2.0.0.0", "2.0") == 0);

    // Newer remote versions
    CHECK(UpdateChecker::compareVersions("1.0.6", "1.0.5") == 1);
    CHECK(UpdateChecker::compareVersions("1.1.0", "1.0.5") == 1);
    CHECK(UpdateChecker::compareVersions("2.0.0", "1.99.99") == 1);
    CHECK(UpdateChecker::compareVersions("1.0.5.1", "1.0.5") == 1);
    CHECK(UpdateChecker::compareVersions("1.0.5-beta", "1.0.4") == 1);

    // Older remote versions
    CHECK(UpdateChecker::compareVersions("1.0.4", "1.0.5") == -1);
    CHECK(UpdateChecker::compareVersions("0.9.9", "1.0.0") == -1);
    CHECK(UpdateChecker::compareVersions("1.0.4.9", "1.0.5") == -1);

    // Empty or edge cases
    CHECK(UpdateChecker::compareVersions("", "1.0.0") == -1);
    CHECK(UpdateChecker::compareVersions("1.0.0", "") == 1);
}

