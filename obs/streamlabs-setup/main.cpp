// hearaside-streamlabs.exe: puts the HEARASIDE source into Streamlabs Desktop, or takes it out again.
//   hearaside-streamlabs check     <Streamlabs folder>                 0 = can be added (2 = yes, but it is running)
//   hearaside-streamlabs install   <Streamlabs folder> <hearaside-obs.dll>
//   hearaside-streamlabs repair    <Streamlabs folder> <hearaside-obs.dll>   install again after a Streamlabs update
//   hearaside-streamlabs uninstall <Streamlabs folder>
// Streamlabs only offers and keeps built-in source types, so besides the plug-in (the OBS one: it fills
// Streamlabs' libobs layout itself) the renderer bundle in resources\app.asar gets seven small additions; the
// original is kept as app.asar.before-hearaside, and <folder>\force-local-bundles makes Streamlabs run that
// bundle instead of the one it downloads at start-up (checked against an MD5). A Streamlabs update replaces all
// of it: "repair" (the installer's scheduled task) puts it back, from the new original, once Streamlabs is closed.
// Exit codes: 0 ok, 1 usage, 2 Streamlabs is running, 3 Streamlabs version not supported, 4 not found, 5 failed.
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <iostream>

namespace {

using juce::File;
using juce::String;

const char* const kSourceId = "hearaside_source";

// {anchor, replacement}: each anchor must be found exactly once in the original renderer bundle
const std::pair<const char*, const char*> kEdits[] = {
    // Windows types a scene collection may contain (SourcesNode.removeUnsupported / load)
    { R"("spout_capture","smart_browser_source"],macSources=)",
      R"("spout_capture","smart_browser_source","hearaside_source"],macSources=)" },
    // "Add Source": the allowlist, filtered by what libobs has loaded
    { R"({description:"Spout2 capture",value:"spout_capture"}].filter()",
      R"({description:"Spout2 capture",value:"spout_capture"},{description:"HEARASIDE",value:"hearaside_source"}].filter()" },
    // name, text, icon and group in the "Add Source" showcase (without a group it is not listed)
    { R"(group:"capture"},smart_browser_source:{name:"Reactive Source")",
      R"(group:"capture"},hearaside_source:{name:"HEARASIDE",shortDesc:"DAW audio",description:"What your viewers hear from the HEARASIDE Hub in your DAW: the Stream Mix or a stem, the same source as in OBS.",icon:"fas fa-microphone",group:"av"},smart_browser_source:{name:"Reactive Source")" },
    // audio-only sources open Advanced Audio, whose panel takes the first property for a device list:
    // no device panel for HEARASIDE, and its own Properties (bus, output, buffer) open as for a video source
    { R"(hasDevices=!((null==source?void 0:source.video)||"soundtrack_source"===(null==source?void 0:source.type)))",
      R"(hasDevices=!((null==source?void 0:source.video)||"soundtrack_source"===(null==source?void 0:source.type)||"hearaside_source"===(null==source?void 0:source.type)))" },
    { R"(!(null==source?void 0:source.video)&&(null==source?void 0:source.hasProps())?AudioService.actions.showAdvancedSettings(source.sourceId))",
      R"(!(null==source?void 0:source.video)&&"hearaside_source"!==(null==source?void 0:source.type)&&(null==source?void 0:source.hasProps())?AudioService.actions.showAdvancedSettings(source.sourceId))" },
    { R"("scene"!==item.type?item.video?)", R"("scene"!==item.type?item.video||"hearaside_source"===item.type?)" },
    { R"(showProperties(){this.options.showAudioMixerMenu||!this.source.video?this.audioService.actions.showAdvancedSettings(this.source.sourceId))",
      R"(showProperties(){this.options.showAudioMixerMenu||!this.source.video&&"hearaside_source"!==this.source.type?this.audioService.actions.showAdvancedSettings(this.source.sourceId))" },
};

struct Paths {
    File root, resources, asar, backup, temp, obsDll, pluginDir, plugin, forceLocal, state;
    explicit Paths(const File& r)
        : root(r), resources(r.getChildFile("resources")), asar(resources.getChildFile("app.asar")),
          backup(resources.getChildFile("app.asar.before-hearaside")), temp(resources.getChildFile("app.asar.hearaside-tmp")),
          obsDll(resources.getChildFile("app.asar.unpacked/node_modules/obs-studio-node/obs.dll")),
          pluginDir(obsDll.getSiblingFile("obs-plugins").getChildFile("64bit")), plugin(pluginDir.getChildFile("hearaside-obs.dll")),
          forceLocal(r.getChildFile("force-local-bundles")), state(resources.getChildFile("app.asar.hearaside-state")) {}
};

struct Failure { int code; String message; };

bool streamlabsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e { sizeof(e) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, L"Streamlabs OBS.exe") == 0;
    CloseHandle(snap);
    return found;
}

juce::var integrity(const juce::MemoryBlock& data) {
    constexpr size_t block = 4 * 1024 * 1024;
    auto* o = new juce::DynamicObject();
    o->setProperty("algorithm", "SHA256");
    o->setProperty("hash", juce::SHA256(data).toHexString().removeCharacters(" "));
    o->setProperty("blockSize", int(block));
    juce::Array<juce::var> blocks;
    for (size_t i = 0; i < juce::jmax<size_t>(data.getSize(), 1); i += block) {
        const auto len = juce::jmin(block, data.getSize() - i);
        blocks.add(juce::SHA256(static_cast<const char*>(data.getData()) + i, len).toHexString().removeCharacters(" "));
    }
    o->setProperty("blocks", blocks);
    return o;
}

void collectFiles(const juce::var& node, const String& prefix, juce::Array<std::pair<String, juce::DynamicObject*>>& out) {
    auto* files = node.getProperty("files", {}).getDynamicObject();
    if (files == nullptr) return;
    for (auto& p : files->getProperties()) {
        auto* entry = p.value.getDynamicObject();
        if (entry == nullptr) continue;
        if (entry->hasProperty("files")) collectFiles(p.value, prefix + p.name.toString() + "/", out);
        else if (entry->hasProperty("offset") && !entry->getProperty("unpacked")) out.add({ prefix + p.name.toString(), entry });
    }
}

// An app.asar's header and its renderer bundle (Streamlabs' UI code)
struct Asar {
    juce::var header;
    int64_t base = 0, off = 0, size = 0;
    juce::Array<std::pair<String, juce::DynamicObject*>> files;
    juce::DynamicObject* bundle = nullptr;
    String text;

    explicit Asar(juce::FileInputStream& in) {
        uint32_t head[4];
        if (in.read(head, 16) != 16) throw Failure { 5, "app.asar is too short" };
        juce::MemoryBlock headerJson;
        in.readIntoMemoryBlock(headerJson, int64_t(head[3]));
        header = juce::JSON::parse(headerJson.toString());
        base = 8 + int64_t(head[1]);
        collectFiles(header, {}, files);
        for (auto& f : files)
            if (f.first.startsWith("bundles/renderer.") && f.first.endsWith(".js")) {
                if (bundle != nullptr) throw Failure { 3, "more than one renderer bundle in app.asar" };
                bundle = f.second;
            }
        if (bundle == nullptr) throw Failure { 3, "no renderer bundle in app.asar" };
        off = bundle->getProperty("offset").toString().getLargeIntValue();
        size = int64_t(bundle->getProperty("size"));
        juce::MemoryBlock original;
        in.setPosition(base + off);
        in.readIntoMemoryBlock(original, size);
        text = String::fromUTF8(static_cast<const char*>(original.getData()), int(original.getSize()));
    }
    bool patched() const { return text.contains(kSourceId); }
};

bool isPatched(const File& asar) {
    juce::FileInputStream in(asar);
    if (!in.openedOk()) throw Failure { 5, "cannot read " + asar.getFullPathName() };
    return Asar(in).patched();
}

String patchedText(String text) {
    for (const auto& [anchor, replacement] : kEdits) {
        const String a = String::fromUTF8(anchor);
        const int first = text.indexOf(a);
        if (first < 0 || text.indexOf(first + 1, a) >= 0)
            throw Failure { 3, "this Streamlabs version is not supported (its UI code changed)" };
        text = text.replace(a, String::fromUTF8(replacement));
    }
    return text;
}

// src (an original app.asar) -> dst with the renderer bundle patched; every other file byte for byte
void patchAsar(const File& src, const File& dst) {
    juce::FileInputStream in(src);
    if (!in.openedOk()) throw Failure { 5, "cannot read " + src.getFullPathName() };
    Asar asar(in);
    if (asar.patched()) throw Failure { 5, src.getFullPathName() + " is not an original app.asar" };
    auto& header = asar.header;
    auto* bundle = asar.bundle;
    auto& files = asar.files;
    const int64_t base = asar.base, off = asar.off, size = asar.size;
    const String text = patchedText(asar.text);
    const juce::MemoryBlock patched(text.toRawUTF8(), text.getNumBytesAsUTF8());
    const int64_t delta = int64_t(patched.getSize()) - size;

    bundle->setProperty("size", juce::var(int64_t(patched.getSize())));
    if (bundle->hasProperty("integrity")) bundle->setProperty("integrity", integrity(patched));
    for (auto& f : files) {
        const int64_t o = f.second->getProperty("offset").toString().getLargeIntValue();
        if (o > off) f.second->setProperty("offset", String(o + delta));
    }

    const String js = juce::JSON::toString(header, true);
    const auto jsLen = uint32_t(js.getNumBytesAsUTF8());
    const uint32_t pad = (4 - jsLen % 4) % 4, payload = 4 + jsLen + pad;
    dst.deleteFile();
    juce::FileOutputStream out(dst);
    if (!out.openedOk()) throw Failure { 5, "cannot write " + dst.getFullPathName() };
    const uint32_t newHead[4] = { 4, payload + 4, payload, jsLen };
    out.write(newHead, 16);
    out.write(js.toRawUTF8(), jsLen);
    const char zeros[4] {};
    out.write(zeros, pad);
    auto copy = [&](int64_t from, int64_t count) {
        in.setPosition(from);
        juce::HeapBlock<char> buf(1 << 20);
        while (count > 0) {
            const int n = in.read(buf, int(juce::jmin<int64_t>(count, 1 << 20)));
            if (n <= 0) throw Failure { 5, "app.asar ended early" };
            out.write(buf, size_t(n));
            count -= n;
        }
    };
    copy(base, off);                                                 // files before the bundle
    out.write(patched.getData(), patched.getSize());
    copy(base + off + size, src.getSize() - (base + off + size));    // files after it, unchanged
    out.flush();
    if (out.getStatus().failed()) throw Failure { 5, "writing failed: " + out.getStatus().getErrorMessage() };
}

// app.asar as we left it (size + time): a Streamlabs update or repair changes it
String asarStamp(const File& asar) { return String(asar.getSize()) + " " + String(asar.getLastModificationTime().toMilliseconds()); }

bool sameFile(const File& a, const File& b) {
    juce::FileInputStream x(a), y(b);
    return a.getSize() == b.getSize() && juce::SHA256(x).toHexString() == juce::SHA256(y).toHexString();
}

void install(const Paths& p, const File& dll) {
    if (!dll.existsAsFile()) throw Failure { 4, "plug-in not found: " + dll.getFullPathName() };
    // An unpatched app.asar is the newest original (first install, or Streamlabs updated): it becomes the
    // backup. A patched one was made from the backup we kept. Never put an older original into a newer Streamlabs.
    if (!isPatched(p.asar)) {
        patchAsar(p.asar, p.temp);
        if (!p.asar.copyFileTo(p.backup)) throw Failure { 5, "cannot keep the original app.asar" };
    } else {
        if (!p.backup.existsAsFile()) throw Failure { 5, "app.asar is patched but its original is missing" };
        patchAsar(p.backup, p.temp);
    }
    if (!MoveFileExW(p.temp.getFullPathName().toWideCharPointer(), p.asar.getFullPathName().toWideCharPointer(),
                     MOVEFILE_REPLACE_EXISTING))
        throw Failure { 5, "cannot replace app.asar" };
    if (!dll.copyFileTo(p.plugin)) throw Failure { 5, "cannot copy the plug-in to " + p.plugin.getFullPathName() };
    if (!p.forceLocal.create()) throw Failure { 5, "cannot create " + p.forceLocal.getFullPathName() };
    p.state.replaceWithText(asarStamp(p.asar));
    std::cout << "HEARASIDE source installed in Streamlabs Desktop" << std::endl;
}

// Quick when nothing changed (the scheduled task runs it every few minutes)
bool installed(const Paths& p, const File& dll) {
    return p.state.existsAsFile() && p.state.loadFileAsString() == asarStamp(p.asar) && p.forceLocal.existsAsFile()
        && p.plugin.existsAsFile() && sameFile(p.plugin, dll);
}

void uninstall(const Paths& p) {
    p.plugin.deleteFile();
    if (p.backup.existsAsFile()) {
        if (isPatched(p.asar)) {   // our patch of that backup: put it back
            if (!p.backup.moveFileTo(p.asar)) throw Failure { 5, "cannot restore the original app.asar" };
        } else {
            p.backup.deleteFile();   // Streamlabs was updated since: the backup is older than what is there now
        }
    }
    p.forceLocal.deleteFile();
    p.state.deleteFile();
    std::cout << "Streamlabs Desktop restored" << std::endl;
}

// 0 = HEARASIDE can be added (or is): the renderer is patched, or every anchor matches once
int check(const Paths& p) {
    juce::FileInputStream in(p.asar);
    if (!in.openedOk()) throw Failure { 5, "cannot read " + p.asar.getFullPathName() };
    const Asar asar(in);
    if (!asar.patched()) patchedText(asar.text);   // throws 3 when an anchor is missing
    return streamlabsRunning() ? 2 : 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "usage: hearaside-streamlabs check|install|repair|uninstall <Streamlabs folder> [hearaside-obs.dll]" << std::endl;
        return 1;
    }
    const String cmd(argv[1]);
    const Paths p(File(String::fromUTF8(argv[2])));
    try {
        if (!p.asar.existsAsFile() || !p.obsDll.existsAsFile()) throw Failure { 4, "Streamlabs Desktop not found in " + p.root.getFullPathName() };
        if (cmd == "uninstall") { if (streamlabsRunning()) throw Failure { 2, "close Streamlabs Desktop first" }; uninstall(p); return 0; }
        if (cmd == "check") return check(p);
        if ((cmd == "install" || cmd == "repair") && argc >= 4) {
            const File dll(String::fromUTF8(argv[3]));
            if (streamlabsRunning()) throw Failure { 2, "close Streamlabs Desktop first" };
            if (cmd == "repair" && installed(p, dll)) return 0;
            install(p, dll);
            return 0;
        }
        std::cerr << "unknown command" << std::endl;
        return 1;
    } catch (const Failure& f) {
        p.temp.deleteFile();
        std::cerr << f.message << std::endl;
        return f.code;
    }
}
