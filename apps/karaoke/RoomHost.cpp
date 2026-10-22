#include "RoomHost.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace hearaside::karaoke {

RoomHost::RoomHost() {
    hubToken_ = ssbus::randomToken();
    heapBus_ = std::make_unique<ssbus::BusLayout>();
    bus_ = heapBus_.get();
    std::memset(bus_, 0, sizeof(ssbus::BusLayout));
}

RoomHost::~RoomHost() {
    stopRoom();
}

void RoomHost::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max(maxBlockSize, 256);

    scratchStreamL_.resize(static_cast<size_t>(maxBlockSize_));
    scratchStreamR_.resize(static_cast<size_t>(maxBlockSize_));
    scratchFriendL_.resize(static_cast<size_t>(maxBlockSize_));
    scratchFriendR_.resize(static_cast<size_t>(maxBlockSize_));
    monoRef_.resize(static_cast<size_t>(maxBlockSize_));

    for (int i = 0; i < ssbus::kMaxFriends; ++i) {
        friendReaders_[i].prepare(sampleRate_, maxBlockSize_);
    }
}

bool RoomHost::startRoom(const juce::String& roomName, int port) {
    (void)port;
    if (isRunning_.load(std::memory_order_relaxed)) return true;

    roomName_ = roomName.isNotEmpty() ? roomName : "Karaoke Room";

    // K3.3 Collision check: if an active Hub is running on the default bus, fall back to "Karaoke"
    std::string busName = defaultBusName().toStdString();
    ssbus::SharedMemory::Status st{};
    auto testShm = ssbus::SharedMemory::openExisting(busName, st);
    bool collision = false;
    if (testShm && ssbus::hubAlive(testShm->layout(), 3'000'000'000ull)) {
        collision = true;
        busName = "Karaoke";
    }
    dawCollision_.store(collision, std::memory_order_relaxed);

    // Open/create shared memory segment for OBS source
    shm_ = ssbus::SharedMemory::open(busName, st);
    if (shm_) {
        bus_ = &shm_->layout();
    } else {
        bus_ = heapBus_.get();
    }
    std::memset(bus_, 0, sizeof(ssbus::BusLayout));

    // Initialize BusLayout header
    bus_->header.magic.store(ssbus::kMagic, std::memory_order_release);
    bus_->header.version = ssbus::kProtocolVersion;
    bus_->header.layoutSize = sizeof(ssbus::BusLayout);
    bus_->header.hubSampleRate.store(static_cast<uint32_t>(sampleRate_ + 0.5), std::memory_order_relaxed);
    bus_->header.hubOwnerPid.store(ssbus::currentPid(), std::memory_order_relaxed);
    bus_->header.hubHeartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
    timelineCounter_ = 0;

    ssbus::tryClaimHub(*bus_, hubToken_, 3'000'000'000ull);

    // Initialize Stream output for OBS
    bus_->streamHeader.sampleRate = static_cast<uint32_t>(sampleRate_ + 0.5);
    bus_->streamHeader.active.store(1, std::memory_order_relaxed);
    bus_->streamHeader.writePos[0].store(0, std::memory_order_relaxed);

    // Beacon for discovery
    beacon_ = ssbus::BeaconMap::open(busName);
    if (beacon_) ssbus::beaconPublish(beacon_->header());

    // Generate tokens and initialize friends
    listenToken_ = ShareServer::newToken();
    sendToken_ = ShareServer::newToken();
    for (int i = 0; i < ssbus::kMaxFriends; ++i) {
        friendTokens_[i] = ShareServer::newToken();
        friendNames_[i] = "Friend " + juce::String(i + 1);
        bus_->friends[i].id.store(static_cast<uint32_t>(i + 1), std::memory_order_relaxed);
        ssbus::setFriendName(bus_->friends[i], friendNames_[i].toStdString());
        bus_->friends[i].sampleRate.store(static_cast<uint32_t>(sampleRate_ + 0.5), std::memory_order_relaxed);
        bus_->friends[i].writePos.store(0, std::memory_order_relaxed);
        bus_->friends[i].heartbeatNs.store(0, std::memory_order_relaxed);
        slotMuted_[i].store(false, std::memory_order_relaxed);
        friendReaders_[i].reset();
    }

    // Start ShareServer
    if (!shareServer_.start(bus_, listenToken_, sendToken_)) {
        if (shm_) {
            ssbus::releaseHub(*bus_, hubToken_);
            shm_.reset();
            bus_ = heapBus_.get();
        }
        beacon_.reset();
        return false;
    }

    updateFriendLinks();
    isRunning_.store(true, std::memory_order_release);
    if (onRoomStateChanged) onRoomStateChanged();
    return true;
}

void RoomHost::stopRoom() {
    if (!isRunning_.exchange(false, std::memory_order_acq_rel)) return;

    stopRecording();
    shareServer_.stop();

    if (bus_) {
        ssbus::releaseHub(*bus_, hubToken_);
    }
    beacon_.reset();
    shm_.reset();
    bus_ = heapBus_.get();

    for (int i = 0; i < ssbus::kMaxFriends; ++i) {
        friendReaders_[i].reset();
    }

    if (onRoomStateChanged) onRoomStateChanged();
}

void RoomHost::processAudio(const float* vocalL, const float* vocalR,
                            const float* musicL, const float* musicR,
                            float* outHeadphoneL, float* outHeadphoneR,
                            int numSamples) noexcept {
    if (numSamples <= 0) return;

    const bool running = isRunning_.load(std::memory_order_relaxed);
    if (!running || bus_ == nullptr) {
        for (int i = 0; i < numSamples; ++i) {
            const float vL = vocalL ? vocalL[i] : 0.0f;
            const float vR = vocalR ? vocalR[i] : (vocalL ? vocalL[i] : 0.0f);
            const float mL = musicL ? musicL[i] : 0.0f;
            const float mR = musicR ? musicR[i] : (musicL ? musicL[i] : 0.0f);
            if (outHeadphoneL) outHeadphoneL[i] = vL + mL;
            if (outHeadphoneR) outHeadphoneR[i] = vR + mR;
        }
        return;
    }

    // 1. Build stream mix (Host vocal + Music)
    float* streamL = scratchStreamL_.data();
    float* streamR = scratchStreamR_.data();
    float pkL = 0.0f;
    float pkR = 0.0f;

    for (int i = 0; i < numSamples; ++i) {
        const float vL = vocalL ? vocalL[i] : 0.0f;
        const float vR = vocalR ? vocalR[i] : (vocalL ? vocalL[i] : 0.0f);
        const float mL = musicL ? musicL[i] : 0.0f;
        const float mR = musicR ? musicR[i] : (musicL ? musicL[i] : 0.0f);
        const float sL = vL + mL;
        const float sR = vR + mR;
        streamL[i] = sL;
        streamR[i] = sR;
        pkL = std::max(pkL, std::abs(sL));
        pkR = std::max(pkR, std::abs(sR));
    }

    // Write stream mix for OBS
    const float* streamSrc[2] = { streamL, streamR };
    ssbus::ringWrite(bus_->streamAudio[0], bus_->streamHeader.writePos[0], streamSrc, 2, uint32_t(numSamples));
    bus_->streamHeader.peakBits[0][0].store(ssbus::floatBits(pkL), std::memory_order_relaxed);
    bus_->streamHeader.peakBits[0][1].store(ssbus::floatBits(pkR), std::memory_order_relaxed);

    // Write friendMixAudio: what friends hear of host & music
    ssbus::ringWrite(bus_->friendMixAudio, bus_->friendMixWrite, streamSrc, 2, uint32_t(numSamples));

    // Headphone out starts with Host vocal + Music
    if (outHeadphoneL) std::memcpy(outHeadphoneL, streamL, size_t(numSamples) * sizeof(float));
    if (outHeadphoneR) std::memcpy(outHeadphoneR, streamR, size_t(numSamples) * sizeof(float));

    // 2. Read each friend's microphone from shared memory and mix into headphones + friendVoiceAudio
    float* friendDst[2] = { scratchFriendL_.data(), scratchFriendR_.data() };

    for (int i = 0; i < ssbus::kMaxFriends; ++i) {
        const bool pulled = friendReaders_[i].pull(bus_->friendAudio[i],
                                                   bus_->friends[i].writePos,
                                                   bus_->friends[i].sampleRate.load(std::memory_order_relaxed),
                                                   friendDst,
                                                   numSamples);
        if (pulled) {
            float fpk = 0.0f;
            for (int s = 0; s < numSamples; ++s) {
                fpk = std::max(fpk, std::max(std::abs(friendDst[0][s]), std::abs(friendDst[1][s])));
            }
            bus_->friends[i].peakBits.store(ssbus::floatBits(fpk), std::memory_order_relaxed);

            // Mix into host headphones if not muted
            if (!slotMuted_[i].load(std::memory_order_relaxed)) {
                if (outHeadphoneL) {
                    for (int s = 0; s < numSamples; ++s) outHeadphoneL[s] += friendDst[0][s];
                }
                if (outHeadphoneR) {
                    for (int s = 0; s < numSamples; ++s) outHeadphoneR[s] += friendDst[1][s];
                }
            }
        }
    }

    // 3. What friends hear of each other: write friendVoiceAudio[k]
    // friendVoiceAudio[k].ch[0] = friend 2k, ch[1] = friend 2k + 1
    for (int k = 0; k < ssbus::kMaxFriends / 2; ++k) {
        // Prepare zero / friend buffers
        const float* voicePair[2] = { scratchFriendL_.data(), scratchFriendR_.data() };
        ssbus::ringWrite(bus_->friendVoiceAudio[k], bus_->friendVoiceWrite[k], voicePair, 2, uint32_t(numSamples));
    }

    // 4. Master recording (if active)
    recordInUse_.store(true, std::memory_order_relaxed);
    if (auto* w = liveWriter_.load(std::memory_order_relaxed)) {
        const float* recCh[2] = { outHeadphoneL, outHeadphoneR };
        w->write(recCh, numSamples);
        recordedSamples_.fetch_add(numSamples, std::memory_order_relaxed);
    }
    recordInUse_.store(false, std::memory_order_relaxed);

    // 5. Update timeline and heartbeat
    timelineCounter_ += uint64_t(numSamples);
    bus_->header.hubHeartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
}

juce::String RoomHost::roomName() const {
    return roomName_;
}

juce::String RoomHost::listenUrl() const {
    return shareServer_.listenUrl();
}

juce::String RoomHost::friendUrl(int slot) const {
    if (slot < 0 || slot >= ssbus::kMaxFriends) return {};
    return shareServer_.friendUrl(friendTokens_[slot]);
}

juce::String RoomHost::friendDeepLink(int slot) const {
    if (slot < 0 || slot >= ssbus::kMaxFriends) return {};
    const auto webUrl = friendUrl(slot);
    if (webUrl.isEmpty()) return {};
    return "hearaside://join?url=" + juce::URL::addEscapeChars(webUrl, true);
}

int RoomHost::serverPort() const {
    return shareServer_.port();
}

int RoomHost::friendCount() const {
    return ssbus::kMaxFriends;
}

HostFriendSlot RoomHost::slotInfo(int slot) const {
    if (slot < 0 || slot >= ssbus::kMaxFriends) return {};
    HostFriendSlot s;
    s.slot = slot;
    s.id = static_cast<uint32_t>(slot + 1);
    s.name = friendNames_[slot];
    s.token = friendTokens_[slot];
    s.isMuted = slotMuted_[slot].load(std::memory_order_relaxed);

    if (bus_ != nullptr) {
        const uint64_t hb = bus_->friends[slot].heartbeatNs.load(std::memory_order_relaxed);
        const uint64_t now = ssbus::nowNs();
        s.isLive = (hb > 0 && now >= hb && (now - hb) < 3'000'000'000ull);
        s.peak = ssbus::bitsFloat(bus_->friends[slot].peakBits.load(std::memory_order_relaxed));
        s.delayMs = ssbus::bitsFloat(bus_->friends[slot].delayBits.load(std::memory_order_relaxed));
    }
    return s;
}

void RoomHost::setSlotMuted(int slot, bool muted) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) {
        slotMuted_[slot].store(muted, std::memory_order_relaxed);
    }
}

void RoomHost::kickSlot(int slot) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) {
        shareServer_.kickFriend(slot);
        friendReaders_[slot].reset();
        if (bus_) {
            bus_->friends[slot].heartbeatNs.store(0, std::memory_order_relaxed);
            bus_->friends[slot].peakBits.store(0, std::memory_order_relaxed);
        }
    }
}

bool RoomHost::startRecording(const juce::File& targetWavFile) {
    stopRecording();
    recordingFile_ = targetWavFile;
    if (recordingFile_.exists()) recordingFile_.deleteFile();
    recordingFile_.getParentDirectory().createDirectory();

    std::unique_ptr<juce::OutputStream> outStream = recordingFile_.createOutputStream();
    if (!outStream) return false;

    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(sampleRate_)
        .withNumChannels(2)
        .withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto baseWriter = wav.createWriterFor(outStream, options);
    if (!baseWriter) return false;

    if (!recordThread_.isThreadRunning()) recordThread_.startThread(juce::Thread::Priority::high);

    recordedSamples_.store(0, std::memory_order_relaxed);
    threadedWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(baseWriter.release(), recordThread_, int(sampleRate_ * 4.0));
    liveWriter_.store(threadedWriter_.get(), std::memory_order_release);
    isRecording_.store(true, std::memory_order_release);
    return true;
}

void RoomHost::stopRecording() {
    if (!isRecording_.exchange(false, std::memory_order_acq_rel)) return;
    liveWriter_.store(nullptr, std::memory_order_release);
    while (recordInUse_.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    threadedWriter_.reset();
    recordThread_.stopThread(2000);
}

double RoomHost::recordedSeconds() const {
    return double(recordedSamples_.load(std::memory_order_relaxed)) / (sampleRate_ > 0 ? sampleRate_ : 48000.0);
}

void RoomHost::updateFriendLinks() {
    std::vector<ShareServer::FriendLink> links;
    for (int i = 0; i < ssbus::kMaxFriends; ++i) {
        ShareServer::FriendLink l;
        l.slot = i;
        l.id = static_cast<uint32_t>(i + 1);
        l.token = friendTokens_[i];
        l.name = friendNames_[i];
        l.host = roomName_;
        links.push_back(l);
    }
    shareServer_.setFriendLinks(std::move(links));
}

} // namespace hearaside::karaoke
