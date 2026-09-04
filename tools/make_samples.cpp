// make_samples.cpp - render proof-of-life WAV files for every Monologue '97
// voice and for the extremes of every speech parameter.
//
//   make_samples <engine-dir> <output-dir>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>

#include <string>
#include <vector>

#include "../src/mono_core.h"
#include "../src/mono_log.h"
#include "../src/mono_voices.h"

namespace {

struct Capture {
  std::vector<int16_t> pcm;
};

bool collect(const int16_t *s, size_t n, void *user) {
  Capture *c = (Capture *)user;
  c->pcm.insert(c->pcm.end(), s, s + n);
  return true;
}

bool write_wav(const std::string &path, const std::vector<int16_t> &pcm,
               int rate) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
  const uint32_t byte_rate = (uint32_t)rate * 2u;
  struct {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format, channels;
    uint32_t rate, byte_rate;
    uint16_t align, bits;
    char data[4];
    uint32_t data_size;
  } h;
  memcpy(h.riff, "RIFF", 4);
  h.riff_size = 36u + data_bytes;
  memcpy(h.wave, "WAVE", 4);
  memcpy(h.fmt, "fmt ", 4);
  h.fmt_size = 16;
  h.format = 1;
  h.channels = 1;
  h.rate = (uint32_t)rate;
  h.byte_rate = byte_rate;
  h.align = 2;
  h.bits = 16;
  memcpy(h.data, "data", 4);
  h.data_size = data_bytes;
  fwrite(&h, sizeof h, 1, f);
  if (data_bytes) fwrite(pcm.data(), 1, data_bytes, f);
  fclose(f);
  return true;
}

// Peak level, so a silent render is reported as a failure rather than shipped.
int peak_of(const std::vector<int16_t> &pcm) {
  int pk = 0;
  for (size_t i = 0; i < pcm.size(); i++) {
    int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
    if (v > pk) pk = v;
  }
  return pk;
}

std::wstring widen(const char *s) {
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  std::wstring w((size_t)n - 1, L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
  return w;
}

int g_ok = 0, g_fail = 0;

bool render_one(mono::Engine &eng, const char *font, const mono::Params &p,
                const std::string &text, const std::string &path,
                bool allow_silent = false) {
  std::string err;
  if (!eng.select_font(font, &err)) {
    printf("  FAIL %-34s select_font: %s\n", path.c_str(), err.c_str());
    g_fail++;
    return false;
  }
  Capture cap;
  if (!eng.render(text, p, collect, &cap, &err)) {
    printf("  FAIL %-34s render: %s\n", path.c_str(), err.c_str());
    g_fail++;
    return false;
  }
  int pk = peak_of(cap.pcm);
  if (cap.pcm.empty() || pk < 200) {
    if (!allow_silent) {
      printf("  FAIL %-34s silent (%u samples, peak %d)\n", path.c_str(),
             (unsigned)cap.pcm.size(), pk);
      g_fail++;
      return false;
    }
    // Silence is the right answer here (volume zero), so keep the file: it is
    // part of demonstrating the parameter's full travel.
    write_wav(path, cap.pcm, eng.sample_rate());
    printf("  ok   %-34s %6.2fs silent, as expected\n", path.c_str(),
           cap.pcm.size() / (double)eng.sample_rate());
    g_ok++;
    return true;
  }
  write_wav(path, cap.pcm, eng.sample_rate());
  printf("  ok   %-34s %6.2fs peak %5d\n", path.c_str(),
         cap.pcm.size() / (double)eng.sample_rate(), pk);
  g_ok++;
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("usage: make_samples <engine-dir> <output-dir>\n");
    return 1;
  }
  mono::log_init("make_samples");
  const std::wstring engdir = widen(argv[1]);
  const std::string out = argv[2];
  CreateDirectoryA(out.c_str(), nullptr);

  mono::Engine eng;
  std::string err;
  if (!eng.init(engdir, &err)) {
    printf("engine init failed: %s\n", err.c_str());
    return 2;
  }
  printf("engine ready, %d Hz\n\n", eng.sample_rate());

  const mono::Params def = mono::default_params();

  // "scan" mode: walk every value of every parameter and report how much audio
  // the engine actually produces, to find values that are accepted but yield
  // nothing.
  if (argc > 3 && strcmp(argv[3], "scan") == 0) {
    eng.select_font("ENMH", &err);
    for (int pi = 0; pi < mono::kParamCount; pi++) {
      const mono::ParamSpec &s = mono::kParams[pi];
      printf("--- %s (%d..%d) ---\n", s.name, s.lo, s.hi);
      for (int v = s.lo; v <= s.hi; v++) {
        mono::Params p = def;
        p.v[pi] = v;
        Capture cap;
        eng.render("The quick brown fox jumps over the lazy dog.", p, collect,
                   &cap, &err);
        printf("  %-10s %4d -> %7u samples  %6.2fs  peak %5d\n", s.name, v,
               (unsigned)cap.pcm.size(),
               cap.pcm.size() / (double)eng.sample_rate(), peak_of(cap.pcm));
      }
    }
    return 0;
  }

  // 1. Every voice, same sentence, default parameters.
  printf("--- voices ---\n");
  for (int i = 0; i < mono::kFontCount; i++) {
    if (!eng.font_available(i)) {
      printf("  skip %s (not installed)\n", mono::kFonts[i].name);
      continue;
    }
    char text[256];
    _snprintf_s(text, _TRUNCATE,
                "This is the %s voice of Monologue ninety seven, "
                "a nineteen ninety seven speech synthesizer by First Byte.",
                mono::kFonts[i].display);
    char path[MAX_PATH];
    _snprintf_s(path, _TRUNCATE, "%s\\voice-%02d-%s.wav", out.c_str(), i + 1,
                mono::kFonts[i].name);
    render_one(eng, mono::kFonts[i].name, def, text, path);
  }

  // 2. Each parameter at 0 %, 50 % and 100 %, on the base male voice, so the
  //    full travel of every control can be heard.
  printf("\n--- parameter sweeps (US Male) ---\n");
  for (int pi = 0; pi < mono::kParamCount; pi++) {
    const mono::ParamSpec &s = mono::kParams[pi];
    const int pcts[3] = {0, 50, 100};
    for (int k = 0; k < 3; k++) {
      if (s.boolean && pcts[k] == 50) continue;  // only off/on for a flag
      mono::Params p = def;
      p.v[pi] = mono::percent_to_native(pi, pcts[k]);
      char text[256];
      _snprintf_s(text, _TRUNCATE,
                  "%s at %d percent. The quick brown fox jumps over the lazy "
                  "dog.",
                  s.label, pcts[k]);
      char path[MAX_PATH];
      _snprintf_s(path, _TRUNCATE, "%s\\param-%s-%03d.wav", out.c_str(),
                  s.name, pcts[k]);
      // Volume at 0 % is meant to be inaudible; everything else must not be.
      const bool silent_ok = (s.id == mono::PARAM_VOLUME && pcts[k] == 0);
      render_one(eng, "ENMH", p, text, path, silent_ok);
    }
  }

  // 3. A couple of combinations, to show the flags stack.
  printf("\n--- combinations ---\n");
  {
    mono::Params p = def;
    p.v[0] = mono::percent_to_native(0, 85);  // fast
    char path[MAX_PATH];
    _snprintf_s(path, _TRUNCATE, "%s\\combo-fast-female.wav", out.c_str());
    render_one(eng, "ENFH", p,
               "Fast speech on the United States female voice, as a screen "
               "reader would normally use it.", path);
  }
  {
    mono::Params p = def;
    p.v[7] = 1;  // breathy
    p.v[9] = 1;  // creaky
    char path[MAX_PATH];
    _snprintf_s(path, _TRUNCATE, "%s\\combo-breathy-creaky.wav", out.c_str());
    render_one(eng, "ENMH", p,
               "Breathy and creaky together on the United States male voice.",
               path);
  }
  {
    mono::Params p = def;
    char path[MAX_PATH];
    _snprintf_s(path, _TRUNCATE, "%s\\combo-numbers-punctuation.wav",
                out.c_str());
    render_one(eng, "ENMH", p,
               "Testing numbers and punctuation: 1997, 3.14159, $42.50, "
               "Mr. Smith paid 75% on May 3rd; isn't that right?", path);
  }

  printf("\n%d rendered, %d failed\n", g_ok, g_fail);
  return g_fail ? 3 : 0;
}
