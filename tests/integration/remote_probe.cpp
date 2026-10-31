// Cross-language probe for the OBS hotkey remote commands (PLAN-rust-migration R4.2). Compiles the real
// obs/hearaside-obs/src/remote.cpp (hearaside::BusClient, no libobs needed), so the Rust hs-obs tests can compare
// what the C++ hotkeys post with what the Rust ones post, and have the C++ Hub-side pollRemote read Rust commands.
//   ss_remote_probe toggle-panic <bus>     what the hearaside.mute_stream hotkey does
//   ss_remote_probe toggle-preview <bus>   what the hearaside.preview hotkey does
//   ss_remote_probe poll <bus>             pollRemote from cursor 0; prints "cmd <target> <paramId> <valueBits hex>"
// The bus must already exist (like OBS, the probe never creates one). Exit 0 = ran.
#include "remote.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace ssbus;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[1], bus = argv[2];
    auto& client = hearaside::BusClient::instance();
    if (mode == "toggle-panic") { client.togglePanic(bus); return 0; }
    if (mode == "toggle-preview") { client.togglePreview(bus); return 0; }
    if (mode == "poll") {
        const bool found = client.with(bus, [](BusLayout& L) {
            uint32_t cursor = 0;
            pollRemote(L, cursor, [](int target, uint32_t pid, float v) {
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof bits);
                std::printf("cmd %d %u %08x\n", target, pid, bits);
            });
        });
        std::printf(found ? "POLLED\n" : "NO BUS\n");
        return 0;
    }
    return 2;
}
