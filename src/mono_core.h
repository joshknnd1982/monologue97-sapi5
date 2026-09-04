// mono_core.h - in-process wrapper around the 32-bit Monologue '97 engine
// (mnvox11.dll). 32-bit builds only; a 64-bit host reaches it through
// mono_host.exe over the pipe.

#pragma once

#include <windows.h>

#include <string>

#include "mono_voices.h"

namespace mono {

// Called with each block of freshly synthesized 16-bit mono PCM at 11025 Hz.
// Return false to abandon the rest of the utterance.
typedef bool (*PcmSink)(const int16_t *samples, size_t count, void *user);

class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // engine_dir must contain mnvox11.dll and an SF subdirectory. Loads the dll,
  // neutralises its modal error boxes and creates the notification window.
  // Every other method must be called on the same thread as init().
  bool init(const std::wstring &engine_dir, std::string *err);

  // Selects a speech font by registry name ("ENMH", "XYLON", ...). Control
  // blocks are cached, so re-selecting a font already opened is free.
  bool select_font(const char *font_name, std::string *err);
  const char *current_font() const { return current_font_; }

  // Which of the 21 fonts are actually present in the SF directory.
  bool font_available(int index) const;

  bool render(const std::string &text, const Params &p, PcmSink sink,
              void *user, std::string *err);

  int sample_rate() const { return sample_rate_; }
  bool ready() const { return dll_ != nullptr; }

 private:
  bool open_font(const char *name, void **out_scb, std::string *err);
  void apply_params(void *scb, const Params &p);

  HMODULE dll_ = nullptr;
  HWND hwnd_ = nullptr;
  std::wstring dir_;
  std::string sf_dir_;  // ANSI: the engine's API is all ANSI
  int sample_rate_ = kSampleRate;
  DWORD thread_ = 0;

  void *scb_[kFontCount];  // cached control block per font
  const char *current_font_ = nullptr;
  void *active_ = nullptr;

  struct Api;
  Api *api_ = nullptr;
};

}  // namespace mono
