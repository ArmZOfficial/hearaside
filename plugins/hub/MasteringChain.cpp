#include "MasteringChain.h"

namespace hearaside {

namespace {

class PluginWindow : public juce::DocumentWindow {
public:
    PluginWindow(const juce::String& title, juce::AudioProcessorEditor* editor)
        : juce::DocumentWindow(title, juce::Colours::black, juce::DocumentWindow::closeButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
    }
    void closeButtonPressed() override { setVisible(false); }
};

} // namespace

struct MasteringChain::Slot {
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription desc;
    std::atomic<bool> bypass { false };
    std::unique_ptr<PluginWindow> window;   // must go before the plug-in

    ~Slot() {
        window.reset();
        if (plugin) plugin->releaseResources();
    }
};

MasteringChain::MasteringChain() {
    juce::addDefaultFormatsToManager(formats_);
}

MasteringChain::~MasteringChain() {
    const juce::SpinLock::ScopedLockType sl(lock_);
    slots_.clear();
}

void MasteringChain::configure(juce::AudioPluginInstance& p) {
    // stereo in / stereo out on the main buses, everything else (side-chains) disabled
    p.disableNonMainBuses();
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0) layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    p.setBusesLayout(layout);
    p.setRateAndBufferSizeDetails(sampleRate_, maxBlock_);
    p.setNonRealtime(false);
    p.prepareToPlay(sampleRate_, maxBlock_);
}

void MasteringChain::prepare(double sampleRate, int maxBlock) {
    std::vector<juce::AudioPluginInstance*> plugins;
    {
        const juce::SpinLock::ScopedLockType sl(lock_);
        sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
        maxBlock_ = juce::jmax(16, maxBlock);
        scratch_.setSize(16, maxBlock_, false, true, false);
        midi_.ensureSize(256);
        prepared_ = true;
        for (auto& s : slots_) plugins.push_back(s->plugin.get());
    }
    // prepareToPlay is called while the audio thread is stopped by the host
    for (auto* p : plugins) configure(*p);
    refreshLatency();
}

void MasteringChain::release() {
    const juce::SpinLock::ScopedLockType sl(lock_);
    for (auto& s : slots_) s->plugin->releaseResources();
    prepared_ = false;
}

juce::Array<juce::File> MasteringChain::findPluginFiles() {
    juce::Array<juce::File> out;
    juce::StringArray roots;
#if JUCE_WINDOWS
    roots.add(juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory).getChildFile("Common Files/VST3").getFullPathName());
    roots.add(juce::File(juce::SystemStats::getEnvironmentVariable("CommonProgramFiles", "C:\\Program Files\\Common Files")).getChildFile("VST3").getFullPathName());
    roots.add(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getParentDirectory().getChildFile("Local/Programs/Common/VST3").getFullPathName());
#elif JUCE_MAC
    roots.add("/Library/Audio/Plug-Ins/VST3");
    roots.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3").getFullPathName());
#endif
    roots.removeDuplicates(true);
    for (const auto& r : roots) {
        const juce::File dir(r);
        if (!dir.isDirectory()) continue;
        // .vst3 is a file or a bundle folder; do not descend into bundles
        for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*.vst3", juce::File::findFilesAndDirectories)) {
            const auto f = entry.getFile();
            bool insideBundle = false;
            for (auto p = f.getParentDirectory(); p != dir && p.getFullPathName().length() > dir.getFullPathName().length(); p = p.getParentDirectory())
                if (p.hasFileExtension(".vst3")) { insideBundle = true; break; }
            if (insideBundle || f.getFileName().containsIgnoreCase("HEARASIDE")) continue;
            if (!out.contains(f)) out.add(f);
        }
    }
#if JUCE_PLUGINHOST_VST && JUCE_WINDOWS
    // VST2: plain .dll files in the usual folders (only the chosen one is ever loaded)
    juce::StringArray vst2Roots { "C:/Program Files/VSTPlugins", "C:/Program Files/Steinberg/VSTPlugins",
                                  "C:/Program Files/Common Files/VST2", "C:/Program Files/Common Files/Steinberg/VST2" };
    for (const auto& extra : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("VST_PATH", {}), ";", ""))
        if (extra.isNotEmpty()) vst2Roots.add(extra);
    vst2Roots.removeDuplicates(true);
    for (const auto& r : vst2Roots) {
        const juce::File dir(r);
        if (!dir.isDirectory()) continue;
        for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*.dll", juce::File::findFiles)) {
            const auto f = entry.getFile();
            if (f.getFileName().containsIgnoreCase("HEARASIDE")) continue;
            if (!out.contains(f)) out.add(f);
        }
    }
#endif
    struct ByName { static int compareElements(const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareIgnoreCase(b.getFileNameWithoutExtension()); } };
    ByName cmp;
    out.sort(cmp);
    return out;
}

juce::OwnedArray<juce::PluginDescription> MasteringChain::typesIn(const juce::File& file) {
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : formats_.getFormats())
        if (f->fileMightContainThisPluginType(file.getFullPathName()))
            f->findAllTypesForFile(types, file.getFullPathName());
    return types;
}

std::unique_ptr<juce::AudioPluginInstance> MasteringChain::create(const juce::PluginDescription& type, juce::String& error) {
    if (type.isInstrument) { error = "instrument"; return nullptr; }
    auto p = formats_.createPluginInstance(type, sampleRate_, maxBlock_, error);
    if (p && p->getTotalNumOutputChannels() == 0) { error = "no audio output"; return nullptr; }
    return p;
}

juce::String MasteringChain::add(const juce::PluginDescription& type) {
    if (size() >= kMaxPlugins) return "full";
    juce::String error;
    auto plugin = create(type, error);
    if (!plugin) return error.isEmpty() ? juce::String("could not load") : error;
    configure(*plugin);
    auto slot = std::make_unique<Slot>();
    slot->desc = type;
    slot->plugin = std::move(plugin);
    {
        const juce::SpinLock::ScopedLockType sl(lock_);
        slots_.push_back(std::move(slot));
    }
    refreshLatency();
    changed.sendChangeMessage();
    return {};
}

juce::String MasteringChain::name(int i) const {
    return i >= 0 && i < size() ? slots_[size_t(i)]->plugin->getName() : juce::String();
}

bool MasteringChain::isBypassed(int i) const { return i >= 0 && i < size() && slots_[size_t(i)]->bypass.load(); }

void MasteringChain::setBypassed(int i, bool b) {
    if (i < 0 || i >= size()) return;
    slots_[size_t(i)]->bypass.store(b);
    refreshLatency();
    changed.sendChangeMessage();
}

void MasteringChain::remove(int i) {
    if (i < 0 || i >= size()) return;
    std::unique_ptr<Slot> gone;
    {
        const juce::SpinLock::ScopedLockType sl(lock_);
        gone = std::move(slots_[size_t(i)]);
        slots_.erase(slots_.begin() + i);
    }
    gone.reset();   // outside the lock: closes the window, releases the plug-in
    refreshLatency();
    changed.sendChangeMessage();
}

void MasteringChain::move(int from, int to) {
    if (from < 0 || from >= size() || to < 0 || to >= size() || from == to) return;
    {
        const juce::SpinLock::ScopedLockType sl(lock_);
        auto s = std::move(slots_[size_t(from)]);
        slots_.erase(slots_.begin() + from);
        slots_.insert(slots_.begin() + to, std::move(s));
    }
    changed.sendChangeMessage();
}

void MasteringChain::showEditor(int i) {
    if (i < 0 || i >= size()) return;
    auto& s = *slots_[size_t(i)];
    if (!s.window) {
        juce::AudioProcessorEditor* ed = s.plugin->hasEditor() ? s.plugin->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor(*s.plugin);
        s.window = std::make_unique<PluginWindow>(s.plugin->getName() + " - HEARASIDE (viewers)", ed);
    }
    s.window->setVisible(true);
    s.window->toFront(true);
}

void MasteringChain::refreshLatency() {
    int total = 0;
    for (auto& s : slots_)
        if (!s->bypass.load()) total += juce::jmax(0, s->plugin->getLatencySamples());
    if (latency_.exchange(total) != total) changed.sendChangeMessage();
}

void MasteringChain::processStream(float* left, float* right, int n) noexcept {
    const juce::SpinLock::ScopedTryLockType tl(lock_);
    if (!tl.isLocked() || !prepared_ || n > maxBlock_) return;   // list being edited: pass through this block
    for (auto& s : slots_) {
        if (s->bypass.load(std::memory_order_relaxed)) continue;
        auto& p = *s->plugin;
        if (playHead_ != lastPlayHead_) p.setPlayHead(playHead_);
        const int numCh = juce::jlimit(2, scratch_.getNumChannels(), juce::jmax(p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
        float* chans[16] = { left, right };
        for (int c = 2; c < numCh; ++c) {
            chans[c] = scratch_.getWritePointer(c);
            juce::FloatVectorOperations::clear(chans[c], n);
        }
        juce::AudioBuffer<float> buf(chans, numCh, n);
        midi_.clear();
        p.processBlock(buf, midi_);
    }
    lastPlayHead_ = playHead_;
}

std::unique_ptr<juce::XmlElement> MasteringChain::toXml() const {
    auto xml = std::make_unique<juce::XmlElement>("MASTERING");
    for (const auto& s : slots_) {
        auto* e = xml->createNewChildElement("PLUGIN");
        e->setAttribute("bypass", s->bypass.load());
        e->addChildElement(s->desc.createXml().release());
        juce::MemoryBlock mb;
        s->plugin->getStateInformation(mb);
        e->createNewChildElement("STATE")->addTextElement(mb.toBase64Encoding());
    }
    return xml;
}

void MasteringChain::fromXml(const juce::XmlElement& xml) {
    std::vector<std::unique_ptr<Slot>> fresh;
    for (auto* e : xml.getChildWithTagNameIterator("PLUGIN")) {
        juce::PluginDescription desc;
        if (auto* d = e->getChildByName("PLUGIN")) desc.loadFromXml(*d);
        juce::String error;
        auto plugin = create(desc, error);
        if (!plugin) continue;   // plug-in uninstalled or failed: skip, keep the rest of the chain
        if (auto* st = e->getChildByName("STATE")) {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding(st->getAllSubText().trim())) plugin->setStateInformation(mb.getData(), int(mb.getSize()));
        }
        configure(*plugin);
        auto slot = std::make_unique<Slot>();
        slot->desc = desc;
        slot->plugin = std::move(plugin);
        slot->bypass.store(e->getBoolAttribute("bypass", false));
        fresh.push_back(std::move(slot));
        if (int(fresh.size()) >= kMaxPlugins) break;
    }
    std::vector<std::unique_ptr<Slot>> old;
    {
        const juce::SpinLock::ScopedLockType sl(lock_);
        old.swap(slots_);
        slots_ = std::move(fresh);
    }
    old.clear();
    refreshLatency();
    changed.sendChangeMessage();
}

} // namespace hearaside
