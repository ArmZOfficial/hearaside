// HEARASIDE share (like Audiomovers LISTENTO): a small web server inside the Hub.
//   /l/<token>   page that plays the Stream Mix (what viewers hear) in any browser
//   /s/<token>   page that sends a browser microphone into the DAW (App Audio "*link*")
// Audio goes over WebSockets as 16-bit PCM ("HRA1" frames). For an internet link the Hub starts
// cloudflared (a free Cloudflare quick tunnel) when it is installed; otherwise the links work on
// the local network.
#pragma once

#include "ssbus/bus.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hearaside {

class ShareServer {
public:
    enum class Tunnel { Off, Missing, Starting, Ready, Failed };

    ShareServer();
    ~ShareServer();

    // message thread
    bool start(ssbus::BusLayout* bus, const juce::String& listenToken, const juce::String& sendToken);
    void stop();
    bool running() const noexcept { return running_.load(); }
    int port() const noexcept { return port_; }
    juce::String lanBase() const;                       // http://192.168.1.5:47810
    juce::String publicBase() const;                    // https://xxxx.trycloudflare.com, "" = none
    juce::String listenUrl() const;                     // best link: public if ready, else LAN
    juce::String localListenUrl() const;                // http://127.0.0.1:47810/l/... for this computer
    juce::String sendUrl() const;
    Tunnel tunnel() const noexcept { return tunnel_.load(); }
    int listeners() const noexcept { return listeners_.load(); }
    bool senderActive() const noexcept { return senders_.load() > 0; }

    // Web pages allowed to open the WebSockets besides this Hub's own addresses (the share web site
    // of the permanent links). Requests without an Origin (App Audio, tools) are always allowed.
    void setAllowedOrigin(const juce::String& origin);
    bool originAllowed(const juce::String& origin) const;

    // ---- friends room (S2) -------------------------------------------------------------------
    // One send-in link per friend: /s/<token>. The table below is what the server checks; it is
    // owned by the Hub's FriendRoom (message thread). A friend who connects again takes over their
    // own slot ("newest wins" inside one slot, never across slots).
    struct FriendLink {
        int slot = -1;           // 0..7 = ssbus friend index
        uint32_t id = 0;         // FriendHeader::id
        juce::String token;      // the secret in the link (never logged)
        juce::String name;       // shown on the friend's page
        juce::String host;       // "<host> invited you to sing" ("" = not shown)
    };
    void setFriendLinks(std::vector<FriendLink> links);
    void kickFriend(int slot);                   // end this friend's connection (removed from the room)
    void remeasureFriend(int slot);              // forget the measured delay and measure again
    // The listen links (/l/<token>) can be switched off without taking the friends away.
    void setListenEnabled(bool on) noexcept { listenEnabled_.store(on); }
    bool listenEnabled() const noexcept { return listenEnabled_.load(); }
    juce::String friendUrl(const juce::String& token) const;     // best link: public if ready, else LAN
    int friendsConnected() const noexcept { return friendsLive_.load(); }

    static juce::String newToken();    // 26 characters from the OS's secure random source (130 bits)
    static juce::String newSecret();   // 64 hex characters (256 bits), proves who owns the permanent links
    static bool secureRandom(void* dst, size_t n);
    static juce::File findCloudflared();

private:
    struct Conn;
    void acceptLoop();
    void serve(Conn&);
    void streamTo(Conn&);
    void receiveFrom(Conn&);
    void friendSession(Conn&, int slot, uint32_t id, const juce::String& token);
    int  friendSlotFor(const juce::String& token, uint32_t* id = nullptr, juce::String* name = nullptr, juce::String* host = nullptr) const;
    void tunnelLoop();
    void reap();

    std::atomic<bool> running_ { false };
    std::unique_ptr<juce::StreamingSocket> listener_;
    std::thread acceptThread_, tunnelThread_;
    std::list<std::unique_ptr<Conn>> conns_;
    std::mutex connsMutex_, writeMutex_;
    ssbus::BusLayout* bus_ = nullptr;
    juce::String listenToken_, sendToken_;
    int port_ = 0;
    std::atomic<int> listeners_ { 0 }, senders_ { 0 };
    std::atomic<uint32_t> senderGen_ { 0 };
    mutable std::mutex friendsMutex_;
    std::vector<FriendLink> friendLinks_;
    std::array<std::atomic<uint32_t>, ssbus::kMaxFriends> friendGen_ {};
    std::array<std::atomic<bool>, ssbus::kMaxFriends> friendKick_ {}, friendRemeasure_ {};
    std::array<std::mutex, ssbus::kMaxFriends> friendWrite_;
    std::atomic<int> friendsLive_ { 0 };
    std::atomic<bool> listenEnabled_ { true };

    std::atomic<Tunnel> tunnel_ { Tunnel::Off };
    struct Tunnelled;                          // the cloudflared process (dies with the DAW, even on a crash)
    std::unique_ptr<Tunnelled> cloudflared_;
    mutable std::mutex urlMutex_;
    juce::String publicBase_, allowedOrigin_;
};

} // namespace hearaside
