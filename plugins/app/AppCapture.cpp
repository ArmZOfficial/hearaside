#include "AppCapture.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>
#endif

namespace hearaside {

AppCapture::AppCapture() : ring_(std::make_unique<ssbus::ChannelRing>()) {}

AppCapture::~AppCapture() { stop(); }

void AppCapture::stop() {
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    quit_.store(false);
    state_.store(State::Idle);
    pid_ = 0;
#ifdef _WIN32
    if (process_) CloseHandle(process_);
#endif
    process_ = nullptr;
}

void AppCapture::start(uint32_t pid, uint32_t sampleRate, bool excludeTree) {
    stop();
    pid_ = pid;
    if (pid == 0) { state_.store(State::NotRunning); return; }
#ifdef _WIN32
    if (!excludeTree) process_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
#endif
    state_.store(State::Starting);
    thread_ = std::thread([this, pid, sampleRate, excludeTree] { run(pid, sampleRate, excludeTree); });
}

bool AppCapture::targetAlive() const noexcept {
#ifdef _WIN32
    return process_ == nullptr || WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

#ifdef _WIN32

namespace {

template <typename T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(0, n)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string exeOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const std::wstring w(path, len);
        out = utf8(w.substr(w.find_last_of(L"\\/") + 1));
    }
    CloseHandle(h);
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// ActivateAudioInterfaceAsync callback (must be agile: it runs on a system thread).
struct ActivateHandler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    LONG refs = 1;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;

    ~ActivateHandler() { CloseHandle(done); }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return ULONG(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (iid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(op->GetActivateResult(&hr, &unk)) && SUCCEEDED(hr) && unk) {
            result = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
            unk->Release();
        } else {
            result = FAILED(hr) ? hr : E_FAIL;
        }
        SetEvent(done);
        return S_OK;
    }
};

} // namespace

std::vector<AppInfo> AppCapture::listAudioApps() {
    std::vector<AppInfo> out;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the host's message thread is usually STA already
    {
        Com<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&en)))) {
            Com<IMMDeviceCollection> devices;
            UINT count = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) && SUCCEEDED(devices->GetCount(&count))) {
                std::set<std::string> seen;
                const std::string self = lower(exeOf(GetCurrentProcessId()));
                for (UINT d = 0; d < count; ++d) {
                    Com<IMMDevice> dev;
                    Com<IAudioSessionManager2> mgr;
                    Com<IAudioSessionEnumerator> sessions;
                    int n = 0;
                    if (FAILED(devices->Item(d, &dev))) continue;
                    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr)))) continue;
                    if (FAILED(mgr->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&n))) continue;
                    for (int i = 0; i < n; ++i) {
                        Com<IAudioSessionControl> ctl;
                        Com<IAudioSessionControl2> ctl2;
                        DWORD pid = 0;
                        if (FAILED(sessions->GetSession(i, &ctl))) continue;
                        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2)))) continue;
                        if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || ctl2->IsSystemSoundsSession() == S_OK) continue;
                        const std::string exe = exeOf(pid);
                        if (exe.empty() || lower(exe) == self || !seen.insert(lower(exe)).second) continue;
                        out.push_back({ exe, exe.size() > 4 && lower(exe.substr(exe.size() - 4)) == ".exe" ? exe.substr(0, exe.size() - 4) : exe });
                    }
                }
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    std::sort(out.begin(), out.end(), [](const AppInfo& a, const AppInfo& b) { return lower(a.name) < lower(b.name); });
    return out;
}

uint32_t AppCapture::findRootProcess(const std::string& exe) {
    if (exe.empty()) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, std::pair<DWORD, std::string>> procs;   // pid -> (parent, exe)
    PROCESSENTRY32W pe { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        procs[pe.th32ProcessID] = { pe.th32ParentProcessID, lower(utf8(pe.szExeFile)) };
    }
    CloseHandle(snap);
    const std::string want = lower(exe);
    for (const auto& [pid, info] : procs) {
        if (info.second != want) continue;
        const auto parent = procs.find(info.first);
        if (parent == procs.end() || parent->second.second != want) return pid;   // nobody above it runs the same program
    }
    return 0;
}

void AppCapture::run(uint32_t pid, uint32_t sampleRate, bool excludeTree) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // same scheduling class as the DAW's audio threads: no gaps when the computer is busy
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    auto fail = [&] { state_.store(State::Failed); };
    {
        AUDIOCLIENT_ACTIVATION_PARAMS ap {};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = excludeTree ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                                                                   : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv {};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);

        auto* handler = new ActivateHandler();
        Com<IActivateAudioInterfaceAsyncOperation> op;
        Com<IAudioClient> client;
        if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op))
            || WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0 || FAILED(handler->result)) {
            handler->Release();
            fail();
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }
        client.p = handler->client;
        handler->Release();

        WAVEFORMATEX wf {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = sampleRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = sampleRate * 8;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Com<IAudioCaptureClient> cap;
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                          | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        // Smallest packets Windows allows (IAudioClient3, e.g. 2.67 ms instead of 10 ms) = less
        // buffering in the plug-in. Not every device / Windows build offers it for loopback.
        bool shortPeriod = false;
        {
            Com<IAudioClient3> c3;
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&c3)))
                && SUCCEEDED(c3->GetSharedModeEnginePeriod(&wf, &def, &fund, &mn, &mx)) && mn > 0 && mn < def)
                shortPeriod = SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                                        mn, &wf, nullptr));
        }
        shortPeriod_.store(shortPeriod);
        if ((!shortPeriod && FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, &wf, nullptr)))
            || FAILED(client->SetEventHandle(ready))
            || FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))
            || FAILED(client->Start())) {
            CloseHandle(ready);
            fail();
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(co)) CoUninitialize();
            return;
        }
        state_.store(State::Running);
        constexpr UINT32 kChunk = 1024;   // de-interleave in fixed chunks: no allocation while capturing
        float l[kChunk], r[kChunk];
        // largest amount delivered per wake-up over ~1 s: how much the plug-in has to keep buffered
        UINT32 burst = 0, windowMax = 0, windowFrames = 0;
        packetFrames_.store(sampleRate / 100);   // 10 ms until measured
        while (!quit_.load(std::memory_order_relaxed)) {
            WaitForSingleObject(ready, 100);
            if (burst > 0) {
                windowMax = std::max(windowMax, burst);
                windowFrames += burst;
                if (windowFrames >= sampleRate) { packetFrames_.store(windowMax); windowMax = 0; windowFrames = 0; }
                else if (windowMax > packetFrames_.load(std::memory_order_relaxed)) packetFrames_.store(windowMax);
            }
            burst = 0;
            UINT32 packet = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bf = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &bf, nullptr, nullptr))) break;
                burst += frames;
                if (bf & AUDCLNT_BUFFERFLAGS_SILENT) {
                    ssbus::ringWrite(*ring_, writePos_, nullptr, 0, frames);
                } else {
                    const float* in = reinterpret_cast<const float*>(data);
                    for (UINT32 done = 0; done < frames;) {
                        const UINT32 m = std::min(kChunk, frames - done);
                        for (UINT32 i = 0; i < m; ++i) { l[i] = in[2 * (done + i)]; r[i] = in[2 * (done + i) + 1]; }
                        const float* src[2] = { l, r };
                        ssbus::ringWrite(*ring_, writePos_, src, 2, m);
                        done += m;
                    }
                }
                cap->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        CloseHandle(ready);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(co)) CoUninitialize();
}

#else   // other platforms: not available

std::vector<AppInfo> AppCapture::listAudioApps() { return {}; }
uint32_t AppCapture::findRootProcess(const std::string&) { return 0; }
void AppCapture::run(uint32_t, uint32_t, bool) { state_.store(State::Failed); }

#endif

} // namespace hearaside
