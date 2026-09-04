// mono_client.h - client side of the mono_host.exe pipe, shared by the x86 and
// x64 SAPI5 dlls.

#pragma once

#include <windows.h>

#include <string>

#include "mono_protocol.h"
#include "mono_voices.h"

namespace mono {

// Called with each PCM block as it arrives. Return false to stop the
// utterance; the client then signals the helper and drains to the end marker,
// so the connection stays usable for the next utterance.
typedef bool (*ClientSink)(const int16_t *samples, size_t count, void *user);

class Client {
 public:
  Client();
  ~Client();

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // Connects, launching mono_host.exe if it is not already running, and
  // verifies the protocol version. Safe to call repeatedly.
  bool ensure_connected(std::string *err);

  bool speak(const char *font, const Params &p, const std::string &text_ansi,
             ClientSink sink, void *user, std::string *err);

  int sample_rate() const { return sample_rate_; }
  void disconnect();

 private:
  bool connect_once(std::string *err);
  bool launch_helper(std::string *err);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE cancel_ = nullptr;
  unsigned session_ = 0;
  int sample_rate_ = kSampleRate;
  CRITICAL_SECTION lock_;
};

// Directory holding the SAPI dll, used to locate mono_host.exe and engine\.
std::wstring module_dir(HMODULE self);
void set_module(HMODULE h);

}  // namespace mono
