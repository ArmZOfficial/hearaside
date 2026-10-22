#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace hearaside::karaoke {

struct ReleaseInfo {
    juce::String currentVersion;
    juce::String latestVersion;
    juce::String downloadUrl;
    juce::String releaseDate;
    juce::String notesTh;
    juce::String notesEn;
    bool hasUpdate = false;
};

class UpdateChecker : private juce::Thread {
public:
    using Callback = std::function<void(const ReleaseInfo&)>;

    UpdateChecker();
    ~UpdateChecker() override;

    // Trigger non-blocking update check in background. Calls callback on message thread.
    void checkForUpdates(Callback onResult);

    // Semver comparison: returns 1 if v1 > v2, -1 if v1 < v2, 0 if equal.
    static int compareVersions(const juce::String& v1, const juce::String& v2);

private:
    void run() override;

    Callback callback_;
    juce::CriticalSection lock_;
};

} // namespace hearaside::karaoke
