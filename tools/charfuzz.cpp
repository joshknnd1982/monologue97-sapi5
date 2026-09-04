// charfuzz.cpp - does any single character kill the engine?
//
// Arrowing through real text feeds the engine every punctuation mark, digit and
// high-ANSI character there is. If one of them faults, mono_host.exe dies and
// the *next* keystroke silently pays a full process relaunch -- which looks
// exactly like a few hundred milliseconds of random lag.
//
// This speaks every character 0x20..0xFF through the pipe, notices when the
// helper goes away, and reports which input did it.
//
//   charfuzz <engine-dir>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>

#include <string>
#include <vector>

#include "../src/mono_client.h"
#include "../src/mono_log.h"
#include "../src/mono_voices.h"

namespace {

size_t g_samples = 0;
bool sink(const int16_t *, size_t n, void *) {
  g_samples += n;
  return true;
}

DWORD helper_pid() {
  // The helper holds a well-known mutex; if we can create it, it is gone.
  HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Monologue97HostMutex");
  if (h) {
    CloseHandle(h);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  mono::log_init("charfuzz");
  mono::Client cl;
  std::string err;
  if (!cl.ensure_connected(&err)) {
    printf("cannot reach the helper: %s\n", err.c_str());
    return 1;
  }
  const mono::Params p = mono::default_params();

  int crashes = 0, empty = 0, spoke = 0;
  for (int c = 0x20; c <= 0xFF; c++) {
    std::string text(1, (char)c);
    g_samples = 0;
    const bool alive_before = helper_pid() != 0;
    if (!cl.speak("ENMH", p, text, sink, nullptr, &err)) {
      Sleep(200);
      const bool alive_after = helper_pid() != 0;
      printf("  0x%02X '%c'  FAILED: %s%s\n", c,
             (c >= 0x20 && c < 0x7F) ? (char)c : '?', err.c_str(),
             (alive_before && !alive_after) ? "   *** HELPER DIED ***" : "");
      crashes++;
      // Reconnect so the sweep can continue past a fatal character.
      cl.disconnect();
      if (!cl.ensure_connected(&err)) {
        printf("  cannot restart the helper: %s\n", err.c_str());
        return 2;
      }
      continue;
    }
    if (g_samples == 0)
      empty++;
    else
      spoke++;
  }

  // Strings that mix the awkward cases, as a real line of text would.
  const char *lines[] = {
      "Hello, world!",
      "C:\\Users\\joshk\\file.txt",
      "a & b < c > d \" e ' f",
      "1997 3.14159 $42.50 75% #1 @home",
      "-- --- ... !!! ??? ***",
      "        ",
      "\t\t",
      "(nested (parens (here)))",
      "e.g. i.e. Mr. Dr. etc.",
      "12345678901234567890123456789012345678901234567890",
  };
  printf("\n--- lines ---\n");
  for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
    g_samples = 0;
    if (!cl.speak("ENMH", p, lines[i], sink, nullptr, &err)) {
      printf("  FAILED %-52s %s\n", lines[i], err.c_str());
      crashes++;
      cl.disconnect();
      cl.ensure_connected(&err);
    } else {
      printf("  ok     %-52s %u samples\n", lines[i], (unsigned)g_samples);
    }
  }

  printf("\n%d characters spoke, %d produced no audio, %d failures\n", spoke,
         empty, crashes);
  return crashes ? 3 : 0;
}
