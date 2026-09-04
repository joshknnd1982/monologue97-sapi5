// loopback.cpp - ground-truth latency: how long after Speak() does sound
// actually leave the speakers?
//
// SAPI's SPEI_START_INPUT_STREAM fires when SAPI begins consuming the stream,
// not when a sample reaches the device, so it under-reports what a user feels.
// This captures the default render endpoint in WASAPI loopback mode and
// timestamps the first frame that is actually audible.
//
//   loopback <monologue_sapi_x86.dll> [voice-token-suffix]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <sapi.h>
#include <stdio.h>

#include <string>

namespace {

double g_freq = 0.0;
double now_ms() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / g_freq;
}

// Set by the main thread just before Speak(); the capture thread reports the
// first audible frame after it.
volatile double g_trigger = 0;
volatile double g_detected = 0;
volatile bool g_stop = false;

IAudioClient *g_client = nullptr;
IAudioCaptureClient *g_capture = nullptr;
WAVEFORMATEX *g_fmt = nullptr;

float frame_peak(const BYTE *data, UINT32 frames, const WAVEFORMATEX *f) {
  float peak = 0;
  const UINT32 ch = f->nChannels;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE || f->wBitsPerSample == 32) {
    const float *p = (const float *)data;
    for (UINT32 i = 0; i < frames * ch; i++) {
      float v = p[i] < 0 ? -p[i] : p[i];
      if (v > peak) peak = v;
    }
  } else if (f->wBitsPerSample == 16) {
    const short *p = (const short *)data;
    for (UINT32 i = 0; i < frames * ch; i++) {
      float v = (p[i] < 0 ? -(float)p[i] : (float)p[i]) / 32768.0f;
      if (v > peak) peak = v;
    }
  }
  return peak;
}

DWORD WINAPI capture_thread(LPVOID) {
  const float kAudible = 0.003f;  // well above a silent-but-active device
  while (!g_stop) {
    UINT32 packet = 0;
    if (FAILED(g_capture->GetNextPacketSize(&packet)) || packet == 0) {
      Sleep(1);
      continue;
    }
    while (packet) {
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(g_capture->GetBuffer(&data, &frames, &flags, nullptr,
                                      nullptr)))
        break;
      const double t = now_ms();
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames) {
        if (g_trigger > 0 && g_detected == 0 &&
            frame_peak(data, frames, g_fmt) > kAudible)
          g_detected = t;
      }
      g_capture->ReleaseBuffer(frames);
      if (FAILED(g_capture->GetNextPacketSize(&packet))) break;
    }
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_freq = (double)f.QuadPart;

  if (argc < 2) {
    printf("usage: loopback <monologue_sapi_x86.dll> [token-suffix]\n");
    return 1;
  }
  const std::wstring suffix = argc > 2 ? argv[2] : L"ENMH";

  typedef HRESULT(STDAPICALLTYPE * PFN_DllInstall)(BOOL, LPCWSTR);
  HMODULE m = LoadLibraryW(argv[1]);
  if (!m) {
    printf("cannot load the dll: %lu\n", GetLastError());
    return 2;
  }
  PFN_DllInstall inst = (PFN_DllInstall)GetProcAddress(m, "DllInstall");
  if (!inst || FAILED(inst(TRUE, L"user"))) {
    printf("per-user registration failed\n");
    return 3;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  // --- loopback capture of the default render endpoint ---
  IMMDeviceEnumerator *en = nullptr;
  IMMDevice *dev = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void **)&en)) ||
      FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    printf("no default render device\n");
    return 4;
  }
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           (void **)&g_client)) ||
      FAILED(g_client->GetMixFormat(&g_fmt))) {
    printf("cannot open the audio client\n");
    return 5;
  }
  if (FAILED(g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0,
                                  g_fmt, nullptr)) ||
      FAILED(g_client->GetService(__uuidof(IAudioCaptureClient),
                                  (void **)&g_capture)) ||
      FAILED(g_client->Start())) {
    printf("cannot start loopback capture\n");
    return 6;
  }
  printf("loopback: %lu Hz, %u ch, %u bit\n", g_fmt->nSamplesPerSec,
         g_fmt->nChannels, g_fmt->wBitsPerSample);
  HANDLE th = CreateThread(nullptr, 0, capture_thread, nullptr, 0, nullptr);
  SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);

  // --- the voice ---
  ISpObjectToken *tok = nullptr;
  std::wstring id =
      L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\"
      L"Monologue97_" +
      suffix;
  ISpVoice *voice = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                              IID_ISpObjectToken, (void **)&tok)) ||
      FAILED(tok->SetId(nullptr, id.c_str(), FALSE)) ||
      FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              (void **)&voice)) ||
      FAILED(voice->SetVoice(tok))) {
    printf("cannot bind the voice\n");
    return 7;
  }

  // Warm the whole path once; the first utterance pays one-off costs.
  voice->Speak(L"warm up", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
  Sleep(1200);

  const wchar_t *chars[] = {L"a", L"b", L"c", L"d", L"e", L"f", L"g", L"h",
                            L"i", L"j", L"k", L"l"};
  printf("\n%-6s %16s\n", "key", "keypress->sound");
  double sum = 0, worst = 0;
  int n = 0;
  for (int i = 0; i < 12; i++) {
    g_detected = 0;
    g_trigger = now_ms();
    double t0 = g_trigger;
    voice->Speak(chars[i], SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);

    while (now_ms() - t0 < 700 && g_detected == 0) Sleep(1);
    if (g_detected > 0) {
      double d = g_detected - t0;
      sum += d;
      n++;
      if (d > worst) worst = d;
      printf("%-6ls %14.1fms\n", chars[i], d);
    } else {
      printf("%-6ls %14s\n", chars[i], "no sound detected");
    }
    // Let the utterance finish before the next keystroke, as a slow arrow
    // would; the rapid case is covered by bench.exe.
    Sleep(500);
  }
  if (n)
    printf("\nkeypress to audible sound: mean %.1f ms, worst %.1f ms\n",
           sum / n, worst);

  g_stop = true;
  WaitForSingleObject(th, 2000);
  g_client->Stop();
  voice->Release();
  tok->Release();
  inst(FALSE, L"user");
  CoUninitialize();
  return 0;
}
