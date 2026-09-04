// mono_log.h - shared debug logging for every Monologue '97 SAPI5 component.
//
// Logs go to %LOCALAPPDATA%\Monologue97\logs\<component>-<pid>.log (falling back
// to the temp directory), one file per component per process, so the SAPI dll
// loaded into a screen reader, the 32-bit host and the configuration utility
// each leave their own trail.
//
// The file is opened with plain "a" and _SH_DENYNO. Do NOT add ccs=UTF-8: a
// stream opened that way makes fprintf fail-fast on the first byte that is not
// valid in the stream's encoding, which kills the host process and leaves a
// 3-byte BOM-only log as the only evidence.

#pragma once

#include <windows.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace mono {

inline CRITICAL_SECTION *log_lock() {
  static CRITICAL_SECTION cs;
  static bool init = false;
  if (!init) {
    InitializeCriticalSection(&cs);
    init = true;
  }
  return &cs;
}

inline const char *&log_component() {
  static const char *name = "monologue";
  return name;
}

inline bool &log_enabled() {
  static bool on = true;
  return on;
}

inline FILE *log_file() {
  static FILE *f = nullptr;
  static bool tried = false;
  if (tried) return f;
  tried = true;

  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return nullptr;
  }
  wchar_t path[MAX_PATH * 2];
  _snwprintf_s(path, _TRUNCATE, L"%s\\Monologue97", dir);
  CreateDirectoryW(path, nullptr);
  _snwprintf_s(path, _TRUNCATE, L"%s\\Monologue97\\logs", dir);
  CreateDirectoryW(path, nullptr);

  wchar_t comp[64];
  MultiByteToWideChar(CP_ACP, 0, log_component(), -1, comp, 64);
  wchar_t full[MAX_PATH * 2];
  _snwprintf_s(full, _TRUNCATE, L"%s\\Monologue97\\logs\\%s-%lu.log", dir, comp,
               GetCurrentProcessId());
  f = _wfsopen(full, L"a", _SH_DENYNO);
  return f;
}

inline void logf(const char *fmt, ...) {
  if (!log_enabled()) return;
  FILE *f = log_file();
  if (!f) return;

  SYSTEMTIME st;
  GetLocalTime(&st);

  EnterCriticalSection(log_lock());
  fprintf(f, "%02u:%02u:%02u.%03u [%lu] ", st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds, GetCurrentThreadId());
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
  LeaveCriticalSection(log_lock());
}

// Call once at start-up. `component` must be a literal or otherwise outlive the
// process.
inline void log_init(const char *component) {
  log_component() = component;
  logf("=== %s starting (pid %lu) ===", component, GetCurrentProcessId());
}

}  // namespace mono

#define MONO_LOG(...) ::mono::logf(__VA_ARGS__)
