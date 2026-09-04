// mono_host.cpp - 32-bit helper process that owns the Monologue '97 engine and
// serves synthesis to both SAPI5 dlls over a named pipe.
//
// The engine is driven from exactly one thread (it binds a notification window
// to whichever thread opened it), so connection threads hand their work to a
// single engine thread through a small request slot. Rendering runs far faster
// than real time, so serialising every client behind one engine costs nothing
// perceptible and keeps the 1997 code path completely single-threaded, which is
// the only way it is known to be stable.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#include "mono_core.h"
#include "mono_log.h"
#include "mono_protocol.h"
#include "mono_voices.h"

namespace {

mono::Engine *g_engine = nullptr;
HANDLE g_work_ready = nullptr;   // connection thread -> engine thread
HANDLE g_work_done = nullptr;    // engine thread -> connection thread
CRITICAL_SECTION g_work_lock;    // one outstanding request at a time
volatile LONG g_clients = 0;
volatile bool g_quit = false;
HANDLE g_idle_timer = nullptr;

// The request slot handed to the engine thread.
struct Work {
  std::string font;
  std::string text;
  mono::Params params;
  HANDLE pipe = nullptr;
  HANDLE cancel = nullptr;
  bool ok = false;
  std::string err;
};
Work *g_work = nullptr;

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

bool send_msg(HANDLE h, uint32_t type, const void *body, uint32_t len) {
  MonoHeader hdr = {type, len};
  if (!write_all(h, &hdr, sizeof hdr)) return false;
  return len == 0 || write_all(h, body, len);
}

void send_error(HANDLE h, const std::string &msg) {
  std::vector<char> buf(sizeof(MonoError) + msg.size());
  MonoError e;
  e.length = (uint32_t)msg.size();
  memcpy(buf.data(), &e, sizeof e);
  if (!msg.empty()) memcpy(buf.data() + sizeof e, msg.data(), msg.size());
  send_msg(h, MONO_REP_ERROR, buf.data(), (uint32_t)buf.size());
}

// --- engine thread --------------------------------------------------------

struct SinkCtx {
  HANDLE pipe;
  HANDLE cancel;
  bool failed;
};

bool pcm_sink(const int16_t *samples, size_t count, void *user) {
  SinkCtx *c = (SinkCtx *)user;
  if (c->cancel && WaitForSingleObject(c->cancel, 0) == WAIT_OBJECT_0) {
    MONO_LOG("render cancelled by client");
    return false;
  }
  const uint32_t bytes = (uint32_t)(count * sizeof(int16_t));
  std::vector<char> buf(sizeof(MonoAudioChunk) + bytes);
  MonoAudioChunk ch;
  ch.bytes = bytes;
  memcpy(buf.data(), &ch, sizeof ch);
  memcpy(buf.data() + sizeof ch, samples, bytes);
  if (!send_msg(c->pipe, MONO_REP_AUDIO, buf.data(), (uint32_t)buf.size())) {
    c->failed = true;
    return false;
  }
  return true;
}

DWORD WINAPI engine_thread(LPVOID) {
  MONO_LOG("engine thread %lu started", GetCurrentThreadId());
  for (;;) {
    WaitForSingleObject(g_work_ready, INFINITE);
    if (g_quit) break;
    Work *w = g_work;
    if (!w) continue;

    w->ok = false;
    w->err.clear();
    if (!g_engine->select_font(w->font.c_str(), &w->err)) {
      SetEvent(g_work_done);
      continue;
    }
    SinkCtx ctx = {w->pipe, w->cancel, false};
    w->ok = g_engine->render(w->text, w->params, pcm_sink, &ctx, &w->err);
    if (ctx.failed) w->ok = false;
    SetEvent(g_work_done);
  }
  MONO_LOG("engine thread exiting");
  return 0;
}

// --- connection thread ----------------------------------------------------

void handle_speak(HANDLE pipe, const MonoSpeakRequest &req,
                  const std::string &text) {
  wchar_t evname[64];
  _snwprintf_s(evname, _TRUNCATE, MONO_CANCEL_EVENT_FMT, req.session);
  HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, evname);

  Work w;
  w.font.assign(req.font, strnlen(req.font, sizeof req.font));
  w.text = text;
  w.params = mono::default_params();
  for (uint32_t i = 0; i < req.param_count && i < (uint32_t)mono::kParamCount;
       i++)
    w.params.v[i] = mono::clamp_param((int)i, req.params[i]);
  w.pipe = pipe;
  w.cancel = cancel;

  EnterCriticalSection(&g_work_lock);
  g_work = &w;
  SetEvent(g_work_ready);
  WaitForSingleObject(g_work_done, INFINITE);
  g_work = nullptr;
  LeaveCriticalSection(&g_work_lock);

  if (cancel) CloseHandle(cancel);

  if (!w.ok && !w.err.empty())
    send_error(pipe, w.err);
  else
    send_msg(pipe, MONO_REP_END, nullptr, 0);
}

DWORD WINAPI client_thread(LPVOID param) {
  HANDLE pipe = (HANDLE)param;
  InterlockedIncrement(&g_clients);
  MONO_LOG("client connected (%ld active)", g_clients);

  for (;;) {
    MonoHeader hdr;
    if (!read_all(pipe, &hdr, sizeof hdr)) break;
    if (hdr.size > 8u * 1024u * 1024u) {
      MONO_LOG("oversized message %u, dropping client", hdr.size);
      break;
    }
    std::vector<char> body(hdr.size);
    if (hdr.size && !read_all(pipe, body.data(), hdr.size)) break;

    if (hdr.type == MONO_CMD_HELLO) {
      MonoHelloReply r;
      r.protocol = MONO_PROTOCOL_VERSION;
      r.sample_rate = (uint32_t)g_engine->sample_rate();
      r.bits = mono::kBitsPerSample;
      r.channels = mono::kChannels;
      r.font_count = (uint32_t)mono::kFontCount;
      r.engine_version = 0x010A;
      send_msg(pipe, MONO_REP_HELLO, &r, sizeof r);
    } else if (hdr.type == MONO_CMD_SPEAK) {
      if (body.size() < sizeof(MonoSpeakRequest)) break;
      MonoSpeakRequest req;
      memcpy(&req, body.data(), sizeof req);
      std::string text;
      if (req.text_length &&
          body.size() >= sizeof(MonoSpeakRequest) + req.text_length)
        text.assign(body.data() + sizeof(MonoSpeakRequest), req.text_length);
      handle_speak(pipe, req, text);
    } else if (hdr.type == MONO_CMD_SHUTDOWN) {
      MONO_LOG("shutdown requested");
      g_quit = true;
      SetEvent(g_work_ready);
      break;
    } else {
      MONO_LOG("unknown command %u", hdr.type);
      break;
    }
  }

  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  InterlockedDecrement(&g_clients);
  MONO_LOG("client disconnected (%ld active)", g_clients);
  return 0;
}

// A NULL DACL plus a low integrity label so a client at any integrity level can
// reach the helper. Without the label a helper started by an elevated process
// is invisible to an ordinary one, and nothing reports why.
bool make_pipe_sd(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *sd) {
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, sd,
          nullptr))
    return false;
  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = *sd;
  sa->bInheritHandle = FALSE;
  return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  mono::log_init("host");

  // Engine directory: the helper lives next to it.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  size_t slash = dir.find_last_of(L'\\');
  if (slash != std::wstring::npos) dir.resize(slash);
  std::wstring engdir = dir + L"\\engine";
  if (GetFileAttributesW((engdir + L"\\mnvox11.dll").c_str()) ==
      INVALID_FILE_ATTRIBUTES)
    engdir = dir;  // running from a build tree
  MONO_LOG("engine directory: %ls", engdir.c_str());

  // Only one helper at a time: a second one would race for the pipe name.
  HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\Monologue97HostMutex");
  if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
    MONO_LOG("another helper is already running, exiting");
    return 0;
  }

  InitializeCriticalSection(&g_work_lock);
  g_work_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_work_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  SECURITY_ATTRIBUTES sa;
  PSECURITY_DESCRIPTOR sd = nullptr;
  bool have_sd = make_pipe_sd(&sa, &sd);

  // Create the first pipe instance before the engine loads, so a client that
  // launched us can connect immediately and wait, rather than racing a
  // not-yet-existing pipe name.
  HANDLE first = CreateNamedPipeW(
      MONO_PIPE_NAME, PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
      64 * 1024, 64 * 1024, 0, have_sd ? &sa : nullptr);
  if (first == INVALID_HANDLE_VALUE) {
    MONO_LOG("CreateNamedPipe failed %lu", GetLastError());
    return 1;
  }

  g_engine = new mono::Engine();
  std::string err;
  if (!g_engine->init(engdir, &err)) {
    MONO_LOG("engine init failed: %s", err.c_str());
    // Stay alive briefly so a connecting client gets a real error rather than
    // a vanished pipe.
    HANDLE t = CreateThread(nullptr, 0, client_thread, first, 0, nullptr);
    if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
    return 2;
  }
  CreateThread(nullptr, 0, engine_thread, nullptr, 0, nullptr);
  MONO_LOG("helper ready on %ls", MONO_PIPE_NAME);

  HANDLE pipe = first;
  while (!g_quit) {
    BOOL ok = ConnectNamedPipe(pipe, nullptr) ||
              GetLastError() == ERROR_PIPE_CONNECTED;
    if (!ok) {
      CloseHandle(pipe);
    } else {
      HANDLE t = CreateThread(nullptr, 0, client_thread, pipe, 0, nullptr);
      if (t)
        CloseHandle(t);
      else
        CloseHandle(pipe);
    }
    if (g_quit) break;
    // Keep a listening instance up at all times: between DisconnectNamedPipe
    // and the next CreateNamedPipe there would otherwise be a window in which
    // the pipe name does not resolve and a client fails for no reason.
    pipe = CreateNamedPipeW(
        MONO_PIPE_NAME, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0,
        have_sd ? &sa : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      MONO_LOG("CreateNamedPipe failed %lu", GetLastError());
      break;
    }
  }

  MONO_LOG("helper exiting");
  if (sd) LocalFree(sd);
  if (once) CloseHandle(once);
  return 0;
}
