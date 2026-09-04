// bench.cpp - where does the latency of a single keystroke go?
//
// Measures, with QueryPerformanceCounter (GetTickCount's 15.6 ms tick reports a
// flat "15 ms" for a real 6 ms and would hide exactly what we are looking for):
//
//   * engine time to the first PCM block and to the last one
//   * how much of the produced audio is leading and trailing silence
//   * the same numbers through the pipe to mono_host.exe
//
//   bench <engine-dir> [engine|pipe|all]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sapi.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "../src/mono_client.h"
#include "../src/mono_core.h"
#include "../src/mono_log.h"
#include "../src/mono_voices.h"

namespace {

double g_freq = 0.0;
double now_ms() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / g_freq;
}

struct Cap {
  std::vector<int16_t> pcm;
  double t0 = 0, t_first = -1, t_end = 0;
  int blocks = 0;
};

bool sink(const int16_t *s, size_t n, void *user) {
  Cap *c = (Cap *)user;
  if (c->t_first < 0) c->t_first = now_ms();
  c->pcm.insert(c->pcm.end(), s, s + n);
  c->blocks++;
  return true;
}

// Samples of near-silence at each end, in milliseconds.
const int kSilence = 300;  // |sample| below this counts as silence

void edges(const std::vector<int16_t> &p, double *lead_ms, double *tail_ms) {
  size_t i = 0, j = p.size();
  while (i < p.size() && (p[i] > -kSilence && p[i] < kSilence)) i++;
  while (j > i && (p[j - 1] > -kSilence && p[j - 1] < kSilence)) j--;
  *lead_ms = i * 1000.0 / mono::kSampleRate;
  *tail_ms = (p.size() - j) * 1000.0 / mono::kSampleRate;
}

const char *kTexts[] = {"a", "b", "m", "hello",
                        "The quick brown fox jumps over the lazy dog."};

void bench_engine(const std::wstring &dir) {
  mono::Engine eng;
  std::string err;
  if (!eng.init(dir, &err)) {
    printf("engine init failed: %s\n", err.c_str());
    return;
  }
  if (!eng.select_font("ENMH", &err)) {
    printf("select_font failed: %s\n", err.c_str());
    return;
  }
  const mono::Params p = mono::default_params();

  // Warm up: the very first utterance pays one-off costs.
  Cap warm;
  warm.t0 = now_ms();
  eng.render("warm up", p, sink, &warm, &err);

  printf("%-46s %8s %8s %8s %8s %8s %8s\n", "text", "first", "total", "audio",
         "lead", "tail", "blocks");
  for (int i = 0; i < (int)(sizeof kTexts / sizeof kTexts[0]); i++) {
    // Three runs, report the median-ish (second) to avoid a one-off outlier.
    for (int run = 0; run < 3; run++) {
      Cap c;
      c.t0 = now_ms();
      if (!eng.render(kTexts[i], p, sink, &c, &err)) {
        printf("render failed: %s\n", err.c_str());
        return;
      }
      c.t_end = now_ms();
      if (run != 1) continue;
      double lead = 0, tail = 0;
      edges(c.pcm, &lead, &tail);
      char label[64];
      _snprintf_s(label, _TRUNCATE, "\"%.40s\"", kTexts[i]);
      printf("%-46s %7.1fms %7.1fms %7.0fms %7.0fms %7.0fms %6d\n", label,
             c.t_first - c.t0, c.t_end - c.t0,
             c.pcm.size() * 1000.0 / mono::kSampleRate, lead, tail, c.blocks);
    }
  }
}

struct PCap {
  std::vector<int16_t> pcm;
  double t0 = 0, t_first = -1;
  int blocks = 0;
};

bool psink(const int16_t *s, size_t n, void *user) {
  PCap *c = (PCap *)user;
  if (c->t_first < 0) c->t_first = now_ms();
  c->pcm.insert(c->pcm.end(), s, s + n);
  c->blocks++;
  return true;
}

void bench_pipe() {
  mono::Client cl;
  std::string err;
  if (!cl.ensure_connected(&err)) {
    printf("cannot reach the helper: %s\n", err.c_str());
    return;
  }
  const mono::Params p = mono::default_params();
  PCap warm;
  warm.t0 = now_ms();
  cl.speak("ENMH", p, "warm up", psink, &warm, &err);

  printf("%-46s %8s %8s %8s %8s %8s %8s\n", "text (via pipe)", "first", "total",
         "audio", "lead", "tail", "blocks");
  for (int i = 0; i < (int)(sizeof kTexts / sizeof kTexts[0]); i++) {
    for (int run = 0; run < 3; run++) {
      PCap c;
      c.t0 = now_ms();
      if (!cl.speak("ENMH", p, kTexts[i], psink, &c, &err)) {
        printf("speak failed: %s\n", err.c_str());
        return;
      }
      double t_end = now_ms();
      if (run != 1) continue;
      double lead = 0, tail = 0;
      std::vector<int16_t> &v = c.pcm;
      size_t a = 0, b = v.size();
      while (a < v.size() && v[a] > -kSilence && v[a] < kSilence) a++;
      while (b > a && v[b - 1] > -kSilence && v[b - 1] < kSilence) b--;
      lead = a * 1000.0 / mono::kSampleRate;
      tail = (v.size() - b) * 1000.0 / mono::kSampleRate;
      char label[64];
      _snprintf_s(label, _TRUNCATE, "\"%.40s\"", kTexts[i]);
      printf("%-46s %7.1fms %7.1fms %7.0fms %7.0fms %7.0fms %6d\n", label,
             c.t_first - c.t0, t_end - c.t0,
             c.pcm.size() * 1000.0 / mono::kSampleRate, lead, tail, c.blocks);
    }
  }
}

std::wstring widen(const char *s) {
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
  return w;
}

// --- through real SAPI, to the real audio device --------------------------
//
// This is the number the user actually feels: press a key, hear a sound. It
// includes SAPI's own audio buffering and the device, neither of which the
// engine sees.
void bench_sapi(const std::wstring &dllpath) {
  typedef HRESULT(STDAPICALLTYPE * PFN_DllInstall)(BOOL, LPCWSTR);
  HMODULE m = LoadLibraryW(dllpath.c_str());
  if (!m) {
    printf("cannot load %ls: %lu\n", dllpath.c_str(), GetLastError());
    return;
  }
  PFN_DllInstall inst = (PFN_DllInstall)GetProcAddress(m, "DllInstall");
  if (!inst || FAILED(inst(TRUE, L"user"))) {
    printf("per-user registration failed\n");
    return;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;

  ISpObjectToken *tok = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                              IID_ISpObjectToken, (void **)&tok)) ||
      FAILED(tok->SetId(nullptr,
                        L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\"
                        L"Voices\\Tokens\\Monologue97_ENMH",
                        FALSE))) {
    printf("cannot bind the voice token\n");
    return;
  }
  ISpVoice *voice = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              (void **)&voice)) ||
      FAILED(voice->SetVoice(tok))) {
    printf("cannot create the voice\n");
    return;
  }
  voice->SetInterest(SPFEI(SPEI_START_INPUT_STREAM) |
                         SPFEI(SPEI_END_INPUT_STREAM),
                     SPFEI(SPEI_START_INPUT_STREAM) |
                         SPFEI(SPEI_END_INPUT_STREAM));

  const wchar_t *chars[] = {L"a", L"b", L"c", L"d", L"e", L"f", L"g", L"h",
                            L"i", L"j", L"k", L"l", L"m", L"n", L"o"};
  const int kArrowMs = 80;  // how fast a user actually holds an arrow key

  // 1. One character at a time, waiting for it to finish. This is the
  //    uninterrupted case, and shows how long the device stays busy.
  printf("uninterrupted: Speak(ASYNC), wait for the whole utterance\n");
  printf("%-6s %12s %12s %12s\n", "key", "Speak()", "audio start", "audio end");
  for (int i = 0; i < 5; i++) {
    double t0 = now_ms();
    voice->Speak(chars[i], SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    double t_call = now_ms();
    double t_start = -1, t_end = -1;
    for (int k = 0; k < 4000 && t_end < 0; k++) {
      SPEVENT ev;
      ULONG got = 0;
      while (SUCCEEDED(voice->GetEvents(1, &ev, &got)) && got) {
        if (ev.eEventId == SPEI_START_INPUT_STREAM && t_start < 0)
          t_start = now_ms();
        else if (ev.eEventId == SPEI_END_INPUT_STREAM)
          t_end = now_ms();
        got = 0;
      }
      if (t_end < 0) Sleep(1);
    }
    printf("%-6ls %10.1fms %10.1fms %10.1fms\n", chars[i], t_call - t0,
           t_start - t0, t_end - t0);
  }

  // 2. The case the user is complaining about: hold the arrow key down, so
  //    every keystroke interrupts an utterance that is still playing. What
  //    matters here is how long after the keypress the new character is heard.
  printf("\nrapid arrowing (a keystroke every %d ms, each purging the last)\n",
         kArrowMs);
  printf("%-6s %12s %14s\n", "key", "Speak()", "to audio start");
  double worst = 0, sum = 0;
  int n = 0;
  for (int i = 0; i < 15; i++) {
    SPEVENT drain;
    ULONG g = 0;
    while (SUCCEEDED(voice->GetEvents(1, &drain, &g)) && g) g = 0;

    double t0 = now_ms();
    voice->Speak(chars[i], SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    double t_call = now_ms();

    double t_start = -1;
    while (now_ms() - t0 < kArrowMs) {
      SPEVENT ev;
      ULONG got = 0;
      while (SUCCEEDED(voice->GetEvents(1, &ev, &got)) && got) {
        if (ev.eEventId == SPEI_START_INPUT_STREAM && t_start < 0)
          t_start = now_ms();
        got = 0;
      }
      Sleep(1);
    }
    if (t_start > 0) {
      double d = t_start - t0;
      sum += d;
      n++;
      if (d > worst) worst = d;
      printf("%-6ls %10.1fms %12.1fms\n", chars[i], t_call - t0, d);
    } else {
      printf("%-6ls %10.1fms %12s\n", chars[i], t_call - t0,
             "not started in time");
    }
  }
  if (n)
    printf("\nkeystroke -> audio: mean %.1f ms, worst %.1f ms over %d "
           "keystrokes\n",
           sum / n, worst, n);
  voice->Release();
  tok->Release();
  CoUninitialize();
  inst(FALSE, L"user");
}

}  // namespace

int main(int argc, char **argv) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_freq = (double)f.QuadPart;

  mono::log_init("bench");
  const std::wstring dir = widen(argc > 1 ? argv[1] : ".");
  const std::string mode = argc > 2 ? argv[2] : "all";

  if (mode == "engine" || mode == "all") {
    printf("=== engine, in process ===\n");
    bench_engine(dir);
    printf("\n");
  }
  if (mode == "pipe" || mode == "all") {
    printf("=== through mono_host.exe ===\n");
    bench_pipe();
    printf("\n");
  }
  if (mode == "sapi" || mode == "all") {
    printf("=== through SAPI to the audio device ===\n");
    bench_sapi(argc > 3 ? widen(argv[3]) : L"monologue_sapi_x86.dll");
  }
  return 0;
}
