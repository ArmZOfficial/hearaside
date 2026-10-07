// Mastering plug-ins hosted by the Hub, applied to the viewers' Stream Mix only (after the stream
// level, before peak protection). The DAW's own master chain keeps affecting the headphones.
#pragma once

#include "ssengine/engine.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

class MasteringChain : public ssengine::StreamInsert {
public:
    static constexpr int kMaxPlugins = 8;

    MasteringChain();
    ~MasteringChain() override;

    // ---- message thread -------------------------------------------------------------------------
    void prepare(double sampleRate, int maxBlock);
    void release();

    // Plug-in files in the standard VST3 (and VST2 when hosting is enabled) folders. Not loaded.
    static juce::Array<juce::File> findPluginFiles();
    // Types inside one file (loads the file's factory, not the plug-in). Usually one.
    juce::OwnedArray<juce::PluginDescription> typesIn(const juce::File& file);
    // Creates, prepares and appends a plug-in. Returns an error text on failure.
    juce::String add(const juce::PluginDescription& type);

    int size() const noexcept { return int(slots_.size()); }
    juce::String name(int i) const;
    bool isBypassed(int i) const;
    void setBypassed(int i, bool);
    void remove(int i);
    void move(int from, int to);
    void showEditor(int i);
    int  totalLatencyFrames() const noexcept { return latency_.load(std::memory_order_relaxed); }
    void refreshLatency();

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml(const juce::XmlElement&);

    juce::ChangeBroadcaster changed;   // list / bypass / latency changed

    // ---- audio thread ---------------------------------------------------------------------------
    void setPlayHead(juce::AudioPlayHead* ph) noexcept { playHead_ = ph; }
    void processStream(float* left, float* right, int numFrames) noexcept override;
    int  latencyFrames() const noexcept override { return latency_.load(std::memory_order_relaxed); }

private:
    struct Slot;
    std::unique_ptr<juce::AudioPluginInstance> create(const juce::PluginDescription&, juce::String& error);
    void configure(juce::AudioPluginInstance&);

    juce::AudioPluginFormatManager formats_;
    std::vector<std::unique_ptr<Slot>> slots_;   // structure changes hold lock_
    juce::SpinLock lock_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    bool prepared_ = false;
    juce::AudioBuffer<float> scratch_ { 16, 512 };
    juce::MidiBuffer midi_;
    std::atomic<int> latency_ { 0 };
    juce::AudioPlayHead* playHead_ = nullptr;
    juce::AudioPlayHead* lastPlayHead_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasteringChain)
};

} // namespace hearaside
