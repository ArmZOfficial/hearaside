// HEARASIDE share links: https://<any host>/l/<token> (listen) and /s/<token> (send in). The host
// can be the permanent site (Vercel), a trycloudflare tunnel or an address on the Wi-Fi.
// Pure functions, checked by `ui-snapshot --test-ui`.
#pragma once

#include <juce_core/juce_core.h>

namespace hearaside::links {

enum class Kind { None, Listen, Send };

struct Parsed {
    Kind kind = Kind::None;
    juce::String base;    // "https://host[:port]" as typed
    juce::String token;   // the part after /l/ or /s/
};

inline Parsed parse(const juce::String& typed) {
    Parsed out;
    auto t = typed.trim();
    if (!t.startsWithIgnoreCase("https://") && !t.startsWithIgnoreCase("http://")) return out;
    t = t.upToFirstOccurrenceOf("#", false, false).upToFirstOccurrenceOf("?", false, false).trimCharactersAtEnd("/");
    const int schemeEnd = t.indexOf("://") + 3;
    const int pathStart = t.indexOfChar(schemeEnd, '/');
    if (pathStart < 0) return out;
    const auto host = t.substring(schemeEnd, pathStart);
    if (host.isEmpty() || host.containsAnyOf(" \t\\@")) return out;
    const auto path = t.substring(pathStart);   // "/l/<token>" (a site may sit under a sub-path: take the last two parts)
    const auto parts = juce::StringArray::fromTokens(path, "/", "");
    juce::StringArray kept;
    for (const auto& p : parts) if (p.isNotEmpty()) kept.add(p);
    if (kept.size() < 2) return out;
    const auto kind = kept[kept.size() - 2].toLowerCase(), token = kept[kept.size() - 1];
    if (token.length() < 8 || !token.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")) return out;
    if (kind != "l" && kind != "s") return out;
    out.kind = kind == "l" ? Kind::Listen : Kind::Send;
    out.base = t.substring(0, pathStart);
    out.token = token;
    return out;
}

inline bool isListen(const juce::String& t) { return parse(t).kind == Kind::Listen; }
inline bool isSend(const juce::String& t) { return parse(t).kind == Kind::Send; }

} // namespace hearaside::links
