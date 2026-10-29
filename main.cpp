// VHF 音频录制程序
// 功能：选择采集设备、自定义存储位置、能量阈值自动录音（有声则录，无声则停）
// 文件命名：VHF_<录制开始时间>.wav
// 编译：cl /EHsc /O2 main.cpp ole32.lib uuid.lib comctl32.lib shell32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------------- 控件ID ----------------
#define IDC_COMBO_DEV   101
#define IDC_BTN_REFRESH 102
#define IDC_EDIT_PATH   103
#define IDC_BTN_BROWSE  104
#define IDC_EDIT_TH     105
#define IDC_LBL_TH_HINT 106
#define IDC_BTN_START   107
#define IDC_STATUS      108
#define IDC_LIST_LOG    109
#define IDC_LBL_RMS     110
#define IDC_BAR_LEVEL   111
#define IDT_RMS         1

#define OUT_RATE_LOW    16000   // 低采样率版本的输出采样率（单声道）
#define PRE_BUFFER_MS   500     // 触发前回读的缓冲时长（毫秒）

// 工作线程 -> UI 线程 的异步消息（lParam = 需由 UI 释放的字符串）
#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_STATUS   (WM_APP + 2)

// ---------------- 全局状态 ----------------
static HWND g_hWnd = NULL;
static HWND g_hCombo, g_hPath, g_hThEdit, g_hStartBtn, g_hStatus, g_hLog, g_hRmsLabel, g_hLevelBar;
static std::vector<IMMDevice*> g_devices;
static std::atomic<bool> g_running{ false };
static std::thread g_capThread;
static std::atomic<float> g_threshold{ 0.01f };   // RMS 能量阈值
static std::atomic<float> g_currentRms{ 0.0f };   // 实时 RMS（供 UI 显示）
static const int SILENCE_TIMEOUT_MS = 3000;        // 连续静音超过 3 秒则结束当前文件

// ---------------- 日志与状态 ----------------
static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap; va_start(ap, fmt);
    vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[600];
    swprintf(line, 600, L"[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, buf);
    // 异步投递到 UI 线程，避免工作线程被 SendMessage 阻塞造成死锁
    PostMessageW(g_hWnd, WM_APP_LOG, 0, (LPARAM)_wcsdup(line));
}

static void SetStatus(const wchar_t* s)
{
    PostMessageW(g_hWnd, WM_APP_STATUS, 0, (LPARAM)_wcsdup(s));
}

// ---------------- WAV 写入器 ----------------
struct WavWriter {
    FILE* fp = nullptr;
    DWORD dataSize = 0;
    WORD  fmtTag = 0;
    WORD  channels = 0;
    DWORD sampleRate = 0;
    WORD  blockAlign = 0;
    WORD  bitsPerSample = 0;

    bool Open(const wchar_t* path, DWORD rate, WORD ch)
    {
        // 统一转成 16bit PCM 存储，简单通用
        fmtTag = WAVE_FORMAT_PCM;
        channels = ch;
        sampleRate = rate;
        bitsPerSample = 16;
        blockAlign = channels * bitsPerSample / 8;

        fp = _wfopen(path, L"wb");
        if (!fp) return false;

        fwrite("RIFF", 1, 4, fp);
        DWORD riffSize = 36; fwrite(&riffSize, 4, 1, fp); // 占位，关闭时回填
        fwrite("WAVE", 1, 4, fp);
        fwrite("fmt ", 1, 4, fp);
        DWORD fmtSize = 16; fwrite(&fmtSize, 4, 1, fp);
        fwrite(&fmtTag, 2, 1, fp);
        fwrite(&channels, 2, 1, fp);
        fwrite(&sampleRate, 4, 1, fp);
        DWORD byteRate = sampleRate * blockAlign;
        fwrite(&byteRate, 4, 1, fp);
        fwrite(&blockAlign, 2, 1, fp);
        fwrite(&bitsPerSample, 2, 1, fp);
        fwrite("data", 1, 4, fp);
        DWORD zero = 0; fwrite(&zero, 4, 1, fp); // 占位
        return true;
    }

    void WritePcm16(const short* samples, UINT32 frames)
    {
        if (!fp) return;
        size_t bytes = (size_t)frames * channels * sizeof(short);
        fwrite(samples, 1, bytes, fp);
        dataSize += (DWORD)bytes;
    }

    void Close()
    {
        if (!fp) return;
        fseek(fp, 4, SEEK_SET);
        DWORD riffSize = 36 + dataSize;
        fwrite(&riffSize, 4, 1, fp);
        fseek(fp, 40, SEEK_SET);
        fwrite(&dataSize, 4, 1, fp);
        fclose(fp);
        fp = nullptr;
    }
};

// ---------------- 采样转 float ----------------
static float SampleToFloat(const BYTE* p, WORD tag, WORD bits)
{
    if (tag == WAVE_FORMAT_IEEE_FLOAT || tag == WAVE_FORMAT_EXTENSIBLE) {
        if (bits == 32) return *(const float*)p;
    }
    if (tag == WAVE_FORMAT_PCM || tag == WAVE_FORMAT_EXTENSIBLE) {
        if (bits == 16) return *(const short*)p / 32768.0f;
        if (bits == 32 && tag == WAVE_FORMAT_PCM) return (float)(*(const int*)p / 2147483648.0);
    }
    return 0.0f;
}

// 判断实际子格式（处理 EXTENSIBLE）
static void GetEffectiveFormat(const WAVEFORMATEX* fmt, WORD& tag, WORD& bits)
{
    tag = fmt->wFormatTag;
    bits = fmt->wBitsPerSample;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
        if (ext->SubFormat.Data1 == 3)      tag = WAVE_FORMAT_IEEE_FLOAT; // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        else if (ext->SubFormat.Data1 == 1) tag = WAVE_FORMAT_PCM;
    }
}

// ---------------- 单声道线性插值重采样器 ----------------
// 输入: 任意声道数的 16bit 交错采样; 输出: 16kHz 单声道 16bit
struct MonoResampler {
    double step = 0.0;   // srcRate / dstRate
    double pos = 0.0;    // 相对当前缓冲区（含前置 carry）的读取位置
    short  carry = 0;    // 上一包最后一帧，供跨包插值
    bool   hasCarry = false;

    void Init(DWORD srcRate, DWORD dstRate) { step = (double)srcRate / dstRate; }

    void Process(const short* in, UINT32 frames, int srcCh, std::vector<short>& out)
    {
        out.clear();
        if (frames == 0 || srcCh < 1) return;

        // 混成单声道，并在头部拼接 carry
        std::vector<short> ext;
        ext.reserve(frames + 1);
        if (hasCarry) { ext.push_back(carry); pos += 1.0; }
        for (UINT32 f = 0; f < frames; f++) {
            int sum = 0;
            for (int c = 0; c < srcCh; c++) sum += in[(size_t)f * srcCh + c];
            ext.push_back((short)(sum / srcCh));
        }
        hasCarry = true;
        carry = ext.back();

        size_t N = ext.size();
        while (pos + 1.0 < (double)N) {
            size_t i = (size_t)pos;
            double frac = pos - i;
            double s = ext[i] * (1.0 - frac) + ext[i + 1] * frac;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            out.push_back((short)s);
            pos += step;
        }
        pos -= (double)frames; // 换算到下一包起点（不含 carry），下一包开头 pos += 1
        if (pos < -1.0) pos = -1.0;
    }
};

// ---------------- 采集线程 ----------------
static void CaptureThreadProc(IMMDevice* dev, std::wstring saveDir)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;
    HANDLE hEvent = NULL;
    WavWriter wavHi, wavLo;          // 高采样率原始版 + 16k 单声道版
    MonoResampler resampler;
    std::vector<short> loBuf;
    std::wstring dirHi, dirLo;
    wchar_t subHi[16] = L"48k";
    bool writing = false;
    std::deque<std::vector<short>> preBuf;  // 触发前回读缓冲（16bit 交错采样）
    size_t preBufFrames = 0;
    size_t preBufCap = 0;                   // 容量（帧），按采样率换算
    ULONGLONG lastVoiceTick = 0;

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
    if (FAILED(hr)) { Log(L"激活音频客户端失败 hr=0x%08X", hr); goto cleanup; }

    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) { Log(L"获取混音格式失败 hr=0x%08X", hr); goto cleanup; }

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 200000, 0, mixFmt, NULL); // 20ms 缓冲
    if (FAILED(hr)) { Log(L"初始化音频客户端失败 hr=0x%08X", hr); goto cleanup; }

    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    audioClient->SetEventHandle(hEvent);

    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) { Log(L"获取采集服务失败 hr=0x%08X", hr); goto cleanup; }

    {
        WORD tag, bits;
        GetEffectiveFormat(mixFmt, tag, bits);
        Log(L"采集格式: %d Hz, %d 声道, %d bit, %s",
            mixFmt->nSamplesPerSec, mixFmt->nChannels, bits,
            tag == WAVE_FORMAT_IEEE_FLOAT ? L"Float" : L"PCM");
    }

    // 输出子目录: <存储目录>\<原始kHz>k 和 <存储目录>\16k
    swprintf(subHi, 16, L"%dk", mixFmt->nSamplesPerSec / 1000);
    dirHi = saveDir + L"\\" + subHi;
    dirLo = saveDir + L"\\16k";
    CreateDirectoryW(dirHi.c_str(), NULL);
    CreateDirectoryW(dirLo.c_str(), NULL);
    resampler.Init(mixFmt->nSamplesPerSec, OUT_RATE_LOW);
    preBufCap = (size_t)mixFmt->nSamplesPerSec * PRE_BUFFER_MS / 1000;
    Log(L"双路输出: %s (%dHz %d声道) + %s (16kHz 单声道)",
        dirHi.c_str(), mixFmt->nSamplesPerSec, mixFmt->nChannels, dirLo.c_str());

    hr = audioClient->Start();
    if (FAILED(hr)) { Log(L"启动采集失败 hr=0x%08X", hr); goto cleanup; }

    Log(L"采集已启动，等待声音超过阈值 %.4f ...", g_threshold.load());
    SetStatus(L"状态：监听中（等待声音）");

    {
        WORD tag, bits;
        GetEffectiveFormat(mixFmt, tag, bits);
        WORD bytesPerSample = bits / 8;
        std::vector<short> convBuf;

        while (g_running) {
            DWORD wait = WaitForSingleObject(hEvent, 500);
            if (wait != WAIT_OBJECT_0) continue;

            UINT32 packetLen = 0;
            captureClient->GetNextPacketSize(&packetLen);
            while (packetLen > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &frames, &flags, NULL, NULL);
                if (FAILED(hr)) break;

                // 计算 RMS 能量并转换为 16bit PCM
                convBuf.resize((size_t)frames * mixFmt->nChannels);
                double sumSq = 0.0;
                UINT64 totalSamples = (UINT64)frames * mixFmt->nChannels;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    memset(convBuf.data(), 0, convBuf.size() * sizeof(short));
                } else {
                    for (UINT64 i = 0; i < totalSamples; i++) {
                        float s = SampleToFloat(data + i * bytesPerSample, tag, bits);
                        sumSq += (double)s * s;
                        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                        convBuf[i] = (short)(s * 32767.0f);
                    }
                }
                float rms = totalSamples ? (float)sqrt(sumSq / totalSamples) : 0.0f;
                g_currentRms = rms; // 供 UI 实时显示

                ULONGLONG now = GetTickCount64();
                if (rms > g_threshold.load()) {
                    lastVoiceTick = now;
                    if (!writing) {
                        // 生成文件名 VHF_yyyyMMdd_HHmmss_48.wav / _16.wav
                        SYSTEMTIME st; GetLocalTime(&st);
                        wchar_t base[64], fHi[MAX_PATH], fLo[MAX_PATH];
                        swprintf(base, 64, L"VHF_%04d%02d%02d_%02d%02d%02d",
                                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                        swprintf(fHi, MAX_PATH, L"%s\\%s_%s.wav", dirHi.c_str(), base, subHi);
                        swprintf(fLo, MAX_PATH, L"%s\\%s_16.wav", dirLo.c_str(), base);
                        bool okHi = wavHi.Open(fHi, mixFmt->nSamplesPerSec, mixFmt->nChannels);
                        bool okLo = wavLo.Open(fLo, OUT_RATE_LOW, 1);
                        if (okHi && okLo) {
                            writing = true;
                            Log(L"检测到声音 (RMS=%.4f)，开始录制: %s_%s.wav / %s_16.wav (含 %dms 回读)",
                                rms, base, subHi, base, PRE_BUFFER_MS);
                            SetStatus((std::wstring(L"状态：正在录制 -> ") + base).c_str());
                            // 先把预缓冲（触发前 PRE_BUFFER_MS 毫秒）写入，保证话音开头完整
                            for (auto& pkt : preBuf) {
                                UINT32 pf = (UINT32)(pkt.size() / mixFmt->nChannels);
                                wavHi.WritePcm16(pkt.data(), pf);
                                resampler.Process(pkt.data(), pf, mixFmt->nChannels, loBuf);
                                if (!loBuf.empty()) wavLo.WritePcm16(loBuf.data(), (UINT32)loBuf.size());
                            }
                            preBuf.clear();
                            preBufFrames = 0;
                        } else {
                            if (okHi) wavHi.Close();
                            if (okLo) wavLo.Close();
                            Log(L"无法创建文件: %s", okHi ? fLo : fHi);
                        }
                    }
                }

                if (writing) {
                    wavHi.WritePcm16(convBuf.data(), frames);
                    resampler.Process(convBuf.data(), frames, mixFmt->nChannels, loBuf);
                    if (!loBuf.empty()) wavLo.WritePcm16(loBuf.data(), (UINT32)loBuf.size());
                    if (now - lastVoiceTick > (ULONGLONG)SILENCE_TIMEOUT_MS) {
                        wavHi.Close();
                        wavLo.Close();
                        writing = false;
                        preBuf.clear();   // 已录过的尾部不再进入下一段
                        preBufFrames = 0;
                        Log(L"静音超过 %d 秒，结束录制", SILENCE_TIMEOUT_MS / 1000);
                        SetStatus(L"状态：监听中（等待声音）");
                    }
                } else {
                    // 监听状态：滚动缓存最近 PRE_BUFFER_MS 毫秒的音频
                    preBuf.push_back(convBuf);
                    preBufFrames += frames;
                    while (preBufFrames > preBufCap && !preBuf.empty()) {
                        preBufFrames -= preBuf.front().size() / mixFmt->nChannels;
                        preBuf.pop_front();
                    }
                }

                captureClient->ReleaseBuffer(frames);
                captureClient->GetNextPacketSize(&packetLen);
            }
        }
    }

    if (writing) { wavHi.Close(); wavLo.Close(); Log(L"录制停止，文件已保存"); }
    audioClient->Stop();
    Log(L"采集线程已退出");
    SetStatus(L"状态：已停止");

cleanup:
    if (captureClient) captureClient->Release();
    if (audioClient) audioClient->Release();
    if (mixFmt) CoTaskMemFree(mixFmt);
    if (hEvent) CloseHandle(hEvent);
    CoUninitialize();
}

// ---------------- 设备枚举 ----------------
static void ClearDevices()
{
    for (auto d : g_devices) d->Release();
    g_devices.clear();
    SendMessageW(g_hCombo, CB_RESETCONTENT, 0, 0);
}

static void RefreshDevices()
{
    if (g_running) { Log(L"请先停止采集再刷新设备"); return; }
    ClearDevices();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) { Log(L"创建设备枚举器失败"); return; }

    IMMDeviceCollection* pColl = nullptr;
    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr)) { Log(L"枚举采集设备失败"); pEnum->Release(); return; }

    UINT count = 0;
    pColl->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(pColl->Item(i, &dev))) continue;
        IPropertyStore* props = nullptr;
        std::wstring name = L"未知设备";
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
                name = var.pwszVal;
            }
            PropVariantClear(&var);
            props->Release();
        }
        g_devices.push_back(dev);
        SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    pColl->Release();
    pEnum->Release();

    if (!g_devices.empty()) SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);
    Log(L"发现 %d 个采集设备", (int)g_devices.size());
}

// ---------------- 控件启用状态 ----------------
static void UpdateUiState(bool recording)
{
    EnableWindow(g_hCombo, !recording);
    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_REFRESH), !recording);
    EnableWindow(g_hPath, !recording);
    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_BROWSE), !recording);
    SetWindowTextW(g_hStartBtn, recording ? L"停止采集" : L"开始采集");
}

// ---------------- 开始 / 停止 ----------------
static void StartCapture()
{
    int sel = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_devices.size()) {
        MessageBoxW(g_hWnd, L"请先选择一个采集设备", L"提示", MB_ICONWARNING);
        return;
    }
    wchar_t dir[MAX_PATH];
    GetWindowTextW(g_hPath, dir, MAX_PATH);
    if (wcslen(dir) == 0 || GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hWnd, L"请选择有效的存储目录", L"提示", MB_ICONWARNING);
        return;
    }

    // 阈值输入框内容已在 EN_CHANGE 中实时生效
    g_running = true;
    g_capThread = std::thread(CaptureThreadProc, g_devices[sel], std::wstring(dir));
    UpdateUiState(true);
}

static void StopCapture()
{
    g_running = false;
    if (g_capThread.joinable()) g_capThread.join();
    g_currentRms = 0.0f;
    UpdateUiState(false);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                                     x, y, w, h, hWnd, (HMENU)(INT_PTR)id, NULL, NULL);
            SendMessage(c, WM_SETFONT, (WPARAM)hFont, TRUE);
            return c;
        };

        mk(L"STATIC", L"采集设备:", 0, 12, 15, 70, 20, 0);
        g_hCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 85, 12, 330, 200, IDC_COMBO_DEV);
        mk(L"BUTTON", L"刷新", 0, 425, 11, 70, 24, IDC_BTN_REFRESH);

        mk(L"STATIC", L"存储位置:", 0, 12, 50, 70, 20, 0);
        g_hPath = mk(L"EDIT", L"", ES_AUTOHSCROLL, 85, 47, 330, 24, IDC_EDIT_PATH);
        mk(L"BUTTON", L"浏览...", 0, 425, 46, 70, 24, IDC_BTN_BROWSE);
        // 默认存储位置 = 程序所在目录
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(NULL, exeDir, MAX_PATH);
        wchar_t* p = wcsrchr(exeDir, L'\\'); if (p) *p = 0;
        SetWindowTextW(g_hPath, exeDir);

        mk(L"STATIC", L"能量阈值:", 0, 12, 85, 70, 20, 0);
        g_hThEdit = mk(L"EDIT", L"0.01", ES_AUTOHSCROLL, 85, 82, 90, 24, IDC_EDIT_TH);
        mk(L"STATIC", L"(0.00001 ~ 0.5, 越小越灵敏)", 0, 185, 85, 220, 20, IDC_LBL_TH_HINT);

        mk(L"STATIC", L"当前RMS:", 0, 12, 118, 70, 20, 0);
        g_hRmsLabel = mk(L"STATIC", L"0.00000 (-inf dB)", SS_LEFTNOWORDWRAP, 85, 118, 180, 20, IDC_LBL_RMS);
        g_hLevelBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                      270, 118, 235, 20, hWnd, (HMENU)(INT_PTR)IDC_BAR_LEVEL, NULL, NULL);
        SendMessageW(g_hLevelBar, PBM_SETRANGE32, 0, 1000);

        g_hStartBtn = mk(L"BUTTON", L"开始采集", BS_PUSHBUTTON, 12, 152, 100, 30, IDC_BTN_START);
        g_hStatus = mk(L"STATIC", L"状态：已停止", SS_LEFTNOWORDWRAP, 125, 157, 380, 22, IDC_STATUS);

        g_hLog = mk(L"LISTBOX", L"", WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT,
                    12, 192, 593, 166, IDC_LIST_LOG);

        SetTimer(hWnd, IDT_RMS, 100, NULL); // 100ms 刷新一次 RMS 显示
        return 0;
    }

    case WM_TIMER: {
        if (wParam != IDT_RMS) break;
        float rms = g_currentRms.load();
        float th = g_threshold.load();
        float db = rms > 1e-6f ? 20.0f * log10f(rms) : -120.0f;
        wchar_t buf[96];
        swprintf(buf, 96, L"%.5f (%.1f dB)%s", rms, db, rms >= th ? L"  [触发]" : L"");
        SetWindowTextW(g_hRmsLabel, buf);
        // 电平条: -80dB..0dB -> 0..1000
        int pos = db <= -80.0f ? 0 : (db >= 0.0f ? 1000 : (int)((db + 80.0f) / 80.0f * 1000.0f));
        SendMessageW(g_hLevelBar, PBM_SETPOS, pos, 0);
        // 触发时电平条变红
        SendMessageW(g_hLevelBar, PBM_SETBARCOLOR, 0, rms >= th ? RGB(220, 40, 40) : RGB(30, 160, 60));
        return 0;
    }

    case WM_APP_LOG: {
        wchar_t* line = (wchar_t*)lParam;
        if (line) {
            SendMessageW(g_hLog, LB_ADDSTRING, 0, (LPARAM)line);
            int count = (int)SendMessageW(g_hLog, LB_GETCOUNT, 0, 0);
            SendMessageW(g_hLog, LB_SETTOPINDEX, count - 1, 0);
            free(line);
        }
        return 0;
    }

    case WM_APP_STATUS: {
        wchar_t* s = (wchar_t*)lParam;
        if (s) {
            SetWindowTextW(g_hStatus, s);
            free(s);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_EDIT_TH && HIWORD(wParam) == EN_CHANGE) {
            wchar_t buf[64] = {0};
            GetWindowTextW(g_hThEdit, buf, 63);
            wchar_t* end = nullptr;
            double d = wcstod(buf, &end);
            if (end != buf && d >= 0.00001 && d <= 0.5) g_threshold = (float)d; // 录制中也可实时调整
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDC_BTN_REFRESH: RefreshDevices(); break;
        case IDC_BTN_BROWSE: {
            BROWSEINFOW bi{};
            bi.hwndOwner = hWnd;
            bi.lpszTitle = L"选择音频存储目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_hPath, path);
                CoTaskMemFree(pidl);
            }
            break;
        }
        case IDC_BTN_START:
            if (g_running) StopCapture(); else StartCapture();
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_RMS);
        StopCapture();
        g_currentRms = 0.0f;
        ClearDevices();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VHFRecorderWnd";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"VHF 音频自动录制",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 635, 410,
                             NULL, NULL, hInst, NULL);
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    RefreshDevices();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
