#include "UpdateChecker.h"

#include <juce_events/juce_events.h>

#ifndef HEARASIDE_VERSION
#define HEARASIDE_VERSION "1.0.6"
#endif

namespace hearaside::karaoke {

UpdateChecker::UpdateChecker()
    : juce::Thread("HearasideUpdateChecker") {
}

UpdateChecker::~UpdateChecker() {
    stopThread(2000);
}

void UpdateChecker::checkForUpdates(Callback onResult) {
    const juce::ScopedLock sl(lock_);
    callback_ = std::move(onResult);
    if (!isThreadRunning()) {
        startThread();
    }
}

int UpdateChecker::compareVersions(const juce::String& v1, const juce::String& v2) {
    auto parseParts = [](const juce::String& v) {
        std::vector<int> parts;
        juce::StringArray tokens;
        tokens.addTokens(v.trim(), ".", "");
        for (const auto& t : tokens) {
            juce::String numStr;
            for (int i = 0; i < t.length(); ++i) {
                if (juce::CharacterFunctions::isDigit(t[i])) {
                    numStr += t[i];
                } else {
                    break;
                }
            }
            parts.push_back(numStr.isNotEmpty() ? numStr.getIntValue() : 0);
        }
        return parts;
    };

    auto p1 = parseParts(v1);
    auto p2 = parseParts(v2);

    const size_t maxLen = std::max(p1.size(), p2.size());
    p1.resize(maxLen, 0);
    p2.resize(maxLen, 0);

    for (size_t i = 0; i < maxLen; ++i) {
        if (p1[i] > p2[i]) return 1;
        if (p1[i] < p2[i]) return -1;
    }
    return 0;
}

void UpdateChecker::run() {
    ReleaseInfo info;
    info.currentVersion = HEARASIDE_VERSION;
    info.downloadUrl = "https://hearaside.netlify.app/download";

    const juce::String urls[] = {
        "https://hearaside.netlify.app/content/release.json",
        "https://hearaside.netlify.app/release.json"
    };

    juce::var parsedJson;
    bool fetched = false;

    for (const auto& u : urls) {
        if (threadShouldExit()) return;

        juce::URL url(u);
        auto stream = url.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(4000)
        );

        if (stream != nullptr) {
            const auto text = stream->readEntireStreamAsString();
            if (text.isNotEmpty()) {
                parsedJson = juce::JSON::parse(text);
                if (parsedJson.isObject()) {
                    fetched = true;
                    break;
                }
            }
        }
    }

    if (threadShouldExit()) return;

    if (fetched && parsedJson.isObject()) {
        const juce::String remoteVer = parsedJson.getProperty("version", "").toString().trim();
        info.latestVersion = remoteVer;
        info.releaseDate = parsedJson.getProperty("date", "").toString().trim();

        if (parsedJson.hasProperty("installer")) {
            const juce::String inst = parsedJson.getProperty("installer", "").toString();
            if (inst.startsWith("http")) {
                info.downloadUrl = inst;
            }
        }

        // Parse release notes
        const auto notesVar = parsedJson.getProperty("notes", juce::var());
        if (notesVar.isArray() && notesVar.size() > 0) {
            const auto latestNote = notesVar[0];
            if (latestNote.isObject()) {
                info.notesTh = latestNote.getProperty("th", "").toString();
                info.notesEn = latestNote.getProperty("en", "").toString();
            }
        }

        if (remoteVer.isNotEmpty() && compareVersions(remoteVer, info.currentVersion) > 0) {
            info.hasUpdate = true;
        }
    }

    Callback cb;
    {
        const juce::ScopedLock sl(lock_);
        cb = callback_;
    }

    if (cb && !threadShouldExit()) {
        juce::MessageManager::callAsync([cb, info] {
            cb(info);
        });
    }
}

} // namespace hearaside::karaoke
