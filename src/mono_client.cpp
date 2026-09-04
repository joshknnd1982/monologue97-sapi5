// mono_client.cpp - see mono_client.h.

#include "mono_client.h"

#include <vector>

#include "mono_log.h"

namespace mono {

namespace {

HMODULE g_self = nullptr;

bool write_all(HANDLE h, const void *data, DWORD len) {
  const BYTE *p = (const BYTE *)data;
  while (len) {
    DWORD wrote = 0;
    if (!WriteFile(h, p, len, &wrote, nullptr) || wrote == 0) return false;
    p += wrote;
    len -= wrote;
  }
  return true;
}

bool read_all(HANDLE h, void *data, DWORD len) {
  BYTE *p = (BYTE *)data;
  while (len) {
    DWORD got = 0;
    if (!ReadFile(h, p, len, &got, nullptr) || got == 0) return false;
    p += got;
    len -= got;
  }
  return true;
}

}  // namespace

void set_module(HMODULE h) { g_self = h; }

std::wstring module_dir(HMODULE self) {
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW(self, path, MAX_PATH)) return std::wstring();
  std::wstring s(path);
  size_t slash = s.find_last_of(L'\\');
  if (slash != std::wstring::npos) s.resize(slash);
  return s;
}

Client::Client() { InitializeCriticalSection(&lock_); }

Client::~Client() {
  disconnect();
  if (cancel_) CloseHandle(cancel_);
  DeleteCriticalSection(&lock_);
}

void Client::disconnect() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

// The helper lives next to the SAPI dll. Both the x86 and x64 dll launch the
// same 32-bit exe.
bool Client::launch_helper(std::string *err) {
  std::wstring dir = module_dir(g_self);
  std::wstring exe = dir + L"\\mono_host.exe";
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // The x64 dll is installed in a subdirectory next to the shared helper.
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos)
      exe = dir.substr(0, slash) + L"\\mono_host.exe";
  }
  MONO_LOG("launching helper: %ls", exe.c_str());

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  std::wstring cmd = L"\"" + exe + L"\"";
  std::vector<wchar_t> mut(cmd.begin(), cmd.end());
  mut.push_back(L'\0');
  if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    DWORD e = GetLastError();
    MONO_LOG("CreateProcess failed %lu", e);
    if (err) *err = "cannot start the Monologue speech helper";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool Client::connect_once(std::string *err) {
  pipe_ = CreateFileW(MONO_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, 0, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) return false;
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);

  MonoHeader hdr = {MONO_CMD_HELLO, 0};
  if (!write_all(pipe_, &hdr, sizeof hdr) ||
      !read_all(pipe_, &hdr, sizeof hdr) || hdr.type != MONO_REP_HELLO ||
      hdr.size != sizeof(MonoHelloReply)) {
    MONO_LOG("handshake failed");
    disconnect();
    return false;
  }
  MonoHelloReply r;
  if (!read_all(pipe_, &r, sizeof r)) {
    disconnect();
    return false;
  }
  if (r.protocol != MONO_PROTOCOL_VERSION) {
    // A helper left over from an older install: shut it down and let the
    // caller start ours, or it would keep serving a stale wire format.
    MONO_LOG("helper protocol %u != %u, replacing", r.protocol,
             MONO_PROTOCOL_VERSION);
    MonoHeader bye = {MONO_CMD_SHUTDOWN, 0};
    write_all(pipe_, &bye, sizeof bye);
    disconnect();
    Sleep(150);
    return false;
  }
  sample_rate_ = (int)r.sample_rate;
  MONO_LOG("connected: %u Hz, %u fonts, engine 0x%04X", r.sample_rate,
           r.font_count, r.engine_version);
  if (err) err->clear();
  return true;
}

bool Client::ensure_connected(std::string *err) {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  if (connect_once(err)) return true;
  if (!launch_helper(err)) return false;

  // The helper creates its first pipe instance before loading the engine, so
  // this normally succeeds on the first or second try.
  for (int i = 0; i < 60; i++) {
    Sleep(50);
    if (connect_once(err)) return true;
  }
  MONO_LOG("helper did not come up");
  if (err && err->empty()) *err = "the Monologue speech helper did not start";
  return false;
}

bool Client::speak(const char *font, const Params &p,
                   const std::string &text_ansi, ClientSink sink, void *user,
                   std::string *err) {
  EnterCriticalSection(&lock_);
  struct Unlock {
    CRITICAL_SECTION *cs;
    ~Unlock() { LeaveCriticalSection(cs); }
  } unlock{&lock_};

  if (!ensure_connected(err)) return false;

  // A fresh session id per utterance keeps a stale cancel signal from an
  // abandoned utterance out of the next one.
  session_ = GetCurrentProcessId() * 65536u + (++session_ & 0xFFFF);
  wchar_t evname[64];
  _snwprintf_s(evname, _TRUNCATE, MONO_CANCEL_EVENT_FMT, session_);
  if (cancel_) CloseHandle(cancel_);
  cancel_ = CreateEventW(nullptr, TRUE, FALSE, evname);

  MonoSpeakRequest req;
  memset(&req, 0, sizeof req);
  strncpy_s(req.font, sizeof req.font, font ? font : "ENMH", _TRUNCATE);
  req.param_count = kParamCount;
  for (int i = 0; i < kParamCount && i < 16; i++) req.params[i] = p.v[i];
  req.session = session_;
  req.text_length = (uint32_t)text_ansi.size();

  std::vector<char> msg(sizeof req + text_ansi.size());
  memcpy(msg.data(), &req, sizeof req);
  if (!text_ansi.empty())
    memcpy(msg.data() + sizeof req, text_ansi.data(), text_ansi.size());

  MonoHeader hdr = {MONO_CMD_SPEAK, (uint32_t)msg.size()};
  if (!write_all(pipe_, &hdr, sizeof hdr) ||
      !write_all(pipe_, msg.data(), (DWORD)msg.size())) {
    MONO_LOG("speak request failed to send");
    disconnect();
    if (err) *err = "lost the connection to the speech helper";
    return false;
  }

  bool cancelled = false;
  std::vector<char> body;
  for (;;) {
    if (!read_all(pipe_, &hdr, sizeof hdr)) {
      disconnect();
      if (err) *err = "lost the connection to the speech helper";
      return false;
    }
    if (hdr.type == MONO_REP_END) break;
    if (hdr.size > 8u * 1024u * 1024u) {
      disconnect();
      if (err) *err = "the speech helper sent a malformed reply";
      return false;
    }
    body.resize(hdr.size);
    if (hdr.size && !read_all(pipe_, body.data(), hdr.size)) {
      disconnect();
      if (err) *err = "lost the connection to the speech helper";
      return false;
    }

    if (hdr.type == MONO_REP_AUDIO && body.size() >= sizeof(MonoAudioChunk)) {
      MonoAudioChunk ch;
      memcpy(&ch, body.data(), sizeof ch);
      const int16_t *pcm = (const int16_t *)(body.data() + sizeof ch);
      size_t count = ch.bytes / sizeof(int16_t);
      // Keep draining after a cancel so the pipe returns to a clean state.
      if (!cancelled && sink && !sink(pcm, count, user)) {
        cancelled = true;
        if (cancel_) SetEvent(cancel_);
      }
    } else if (hdr.type == MONO_REP_ERROR &&
               body.size() >= sizeof(MonoError)) {
      MonoError e;
      memcpy(&e, body.data(), sizeof e);
      std::string m(body.data() + sizeof e,
                    body.size() - sizeof e < e.length ? body.size() - sizeof e
                                                      : e.length);
      MONO_LOG("helper error: %s", m.c_str());
      if (err) *err = m;
      return false;
    }
  }
  return true;
}

}  // namespace mono
