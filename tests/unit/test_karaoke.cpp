#include "testing.h"

#include "AudioSettings.h"
#include "JoinClient.h"
#include "VocalChain.h"
#include "ShareServer.h"
#include "ssbus/bus.h"

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

    // Send audio upstream
    std::vector<float> micChunk(480, 0.25f);
    for (int i = 0; i < 5; ++i) {
        client.pushUpstream(micChunk.data(), 480, i * 480);
        if (juce::MessageManager::getInstanceWithoutCreating()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    // Pull downstream (should not crash, executes cleanly)
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
