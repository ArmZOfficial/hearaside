// --editor-soak: opens and closes a plug-in's editor in a window like a DAW does, `cycles` times, and
// reports how long each open took and how the process memory moved (R0.3 criteria 2 and 3).
#pragma once

// Included inside main.cpp's anonymous namespace; the OS headers it needs are included at the top of main.cpp.

double residentMb() {
#if JUCE_WINDOWS
    HsProcessMemoryCounters pmc {};
    pmc.cb = sizeof(pmc);
    void* const currentProcess = reinterpret_cast<void*>(intptr_t(-1));   // GetCurrentProcess()
    return K32GetProcessMemoryInfo(currentProcess, &pmc, sizeof(pmc)) ? double(pmc.workingSetSize) / 1048576.0 : 0.0;
#elif JUCE_MAC
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) == KERN_SUCCESS
        ? double(info.resident_size) / 1048576.0 : 0.0;
#else
    long pages = 0, resident = 0;
    if (auto* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(statm);
    }
    return double(resident) * double(sysconf(_SC_PAGESIZE)) / 1048576.0;
#endif
}

int editorSoak(const juce::String& path, int cycles) {
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);
    auto plugin = load(fm, bundlePath(path));
    check(plugin != nullptr, "plug-in loads");
    if (!plugin) return 1;
    plugin->enableAllBuses();
    plugin->setPlayConfigDetails(2, 2, 48000.0, 256);
    plugin->prepareToPlay(48000.0, 256);
    pump(200);

    const double base = residentMb();
    double firstOpen = 0.0, peak = base;
    std::vector<double> openMs;
    for (int i = 0; i < cycles; ++i) {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditorIfNeeded());
        if (!editor) { check(false, "plug-in creates an editor"); return 1; }
        {
            juce::DocumentWindow window("editor soak", juce::Colours::black, 0);
            window.setSize(editor->getWidth(), editor->getHeight());
            window.setVisible(true);   // map the host window first, as DAWs do, then attach the editor
            window.setContentNonOwned(editor.get(), true);
            openMs.push_back(juce::Time::getMillisecondCounterHiRes() - t0);
            // let the editor draw and run its timers, as a user glancing at it would; HS_SOAK_HOLD_MS keeps the
            // first one open longer (for a screenshot)
            pump(i == 0 ? juce::jmax(250, juce::SystemStats::getEnvironmentVariable("HS_SOAK_HOLD_MS", "0").getIntValue()) : 250);
            peak = std::max(peak, residentMb());
            if (i == 0) firstOpen = residentMb() - base;
            window.clearContentComponent();
        }
        editor.reset();
        pump(50);
    }
    plugin->releaseResources();
    const double end = residentMb();

    auto sorted = openMs;
    std::sort(sorted.begin(), sorted.end());
    std::printf("editor open (create + attach), %d cycles: first %.1f ms, median %.1f ms, max %.1f ms\n",
                cycles, openMs.front(), sorted[sorted.size() / 2], sorted.back());
    std::printf("memory: before %.1f MB, +%.1f MB with the first editor open, peak +%.1f MB, +%.1f MB after %d closes\n",
                base, firstOpen, peak - base, end - base, cycles);
    check(int(openMs.size()) == cycles, "editor opened and closed every cycle without a crash");
    return failures == 0 ? 0 : 1;
}
