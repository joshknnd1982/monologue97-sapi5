// mono_core.cpp - see mono_core.h.
//
// Everything unusual about driving this engine lives here:
//   * OpenSpeech only finds its voices through
//     HKLM\Software\FirstByte\PrimoVOX\SpeechFonts, so a private volatile key
//     is aliased over HKEY_LOCAL_MACHINE for the duration of the call. That
//     needs no administrator rights and leaves the real HKLM untouched.
//   * Each font's registry subkey must carry a Description value; the engine
//     reads it into an uninitialised buffer and faults without one.
//   * OpenSpeech's first argument is an HWND (not an HINSTANCE): it lands in
//     the control block's notification slots.
//   * Audio is pulled, never played: TextToCmd -> OpenBackendCmd ->
//     GetPCMdata -> CloseBackend. The Say() paths either play to the sound card
//     or, with a file backend, deadlock.

#include "mono_core.h"

#include <stdio.h>

#include "mono_log.h"

namespace mono {

namespace {

const char *kFontRegPath = "Software\\FirstByte\\PrimoVOX\\SpeechFonts";
const wchar_t *kViewRoot = L"Software\\Monologue97\\EngineView";

int __stdcall suppress_messagebox(HWND, LPCSTR text, LPCSTR caption, UINT) {
  MONO_LOG("engine MessageBox suppressed: [%s] %s", caption ? caption : "",
           text ? text : "");
  return IDOK;
}

// Redirect one imported function of an already-loaded module.
bool patch_import(HMODULE mod, const char *dll, const char *fn, void *repl) {
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (!rva) return false;
  for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
       imp->Name; imp++) {
    if (_stricmp((const char *)(base + imp->Name), dll) != 0) continue;
    IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA *)(base +
        (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; oft->u1.AddressOfData; oft++, ft++) {
      if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
      IMAGE_IMPORT_BY_NAME *n =
          (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
      if (strcmp((const char *)n->Name, fn) != 0) continue;
      DWORD old = 0;
      if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE,
                          &old))
        return false;
      ft->u1.Function = (ULONG_PTR)repl;
      VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
      return true;
    }
  }
  return false;
}

std::string narrow(const std::wstring &w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

}  // namespace

// The engine's exported ABI. Everything is __stdcall.
struct Engine::Api {
  typedef void *SCB;
  WORD(__stdcall *SpeechVersion)(void);
  SCB(__stdcall *OpenSpeech)(void *hwnd, void *reserved, const char *font);
  int(__stdcall *CloseSpeech)(SCB);
  int(__stdcall *SetSpeechParameter)(SCB, int param, int value);
  int(__stdcall *GetSpeechParameter)(SCB, int param, int *value);
  void *(__stdcall *TextToCmd)(SCB, const char *text, int flags);
  int(__stdcall *OpenBackendCmd)(SCB, void *cmd);
  int(__stdcall *GetPCMdata)(SCB, void *buf, int bytes);
  int(__stdcall *CloseBackend)(SCB);
  int(__stdcall *FreePhoneticsCommandStream)(void *);
  int(__stdcall *ResetSpeech)(SCB);
};

Engine::Engine() {
  for (int i = 0; i < kFontCount; i++) scb_[i] = nullptr;
}

Engine::~Engine() {
  if (api_ && api_->CloseSpeech) {
    for (int i = 0; i < kFontCount; i++)
      if (scb_[i]) api_->CloseSpeech(scb_[i]);
  }
  if (hwnd_) DestroyWindow(hwnd_);
  delete api_;
  if (dll_) FreeLibrary(dll_);
}

bool Engine::init(const std::wstring &engine_dir, std::string *err) {
  dir_ = engine_dir;
  thread_ = GetCurrentThreadId();
  sf_dir_ = narrow(engine_dir) + "\\SF";

  std::wstring dllpath = engine_dir + L"\\mnvox11.dll";
  // The engine loads its speech fonts by absolute path, but keep the directory
  // on the search path so any sibling dependency resolves too.
  SetDllDirectoryW(engine_dir.c_str());
  dll_ = LoadLibraryW(dllpath.c_str());
  if (!dll_) {
    DWORD e = GetLastError();
    MONO_LOG("LoadLibrary(%ls) failed: %lu", dllpath.c_str(), e);
    if (err) *err = "cannot load mnvox11.dll";
    return false;
  }

  // A modal error box from the engine would hang whatever host is speaking.
  if (!patch_import(dll_, "USER32.dll", "MessageBoxA",
                    (void *)suppress_messagebox))
    MONO_LOG("warning: could not hook MessageBoxA");

  api_ = new Api();
  memset(api_, 0, sizeof(*api_));
#define BIND(f)                                                       \
  *(FARPROC *)&api_->f = GetProcAddress(dll_, #f);                    \
  if (!api_->f) {                                                     \
    MONO_LOG("missing export %s", #f);                                \
    if (err) *err = "mnvox11.dll is missing export " #f;              \
    return false;                                                     \
  }
  BIND(SpeechVersion);
  BIND(OpenSpeech);
  BIND(CloseSpeech);
  BIND(SetSpeechParameter);
  BIND(GetSpeechParameter);
  BIND(TextToCmd);
  BIND(OpenBackendCmd);
  BIND(GetPCMdata);
  BIND(CloseBackend);
  BIND(FreePhoneticsCommandStream);
  BIND(ResetSpeech);
#undef BIND

  MONO_LOG("mnvox11 loaded, SpeechVersion=0x%04X, SF=%s",
           api_->SpeechVersion(), sf_dir_.c_str());

  // OpenSpeech stores this window in four notification slots. It must belong to
  // the thread that drives the engine, so it is created here and every engine
  // call is expected on this same thread.
  WNDCLASSW wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"Monologue97Notify";
  RegisterClassW(&wc);
  hwnd_ = CreateWindowExW(0, L"Monologue97Notify", L"Monologue97", 0, 0, 0, 0,
                          0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (!hwnd_) MONO_LOG("warning: notification window not created (%lu)",
                       GetLastError());
  return true;
}

bool Engine::font_available(int index) const {
  if (index < 0 || index >= kFontCount) return false;
  std::string p = sf_dir_ + "\\" + kFonts[index].name + ".DLL";
  return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Publish the font list where the engine expects it, run OpenSpeech under that
// view, and take the alias straight back down: RegOverridePredefKey is
// process-wide, so leaving it in place would redirect unrelated HKLM reads.
bool Engine::open_font(const char *name, void **out_scb, std::string *err) {
  HKEY root = nullptr, fonts = nullptr;
  DWORD disp = 0;
  RegDeleteTreeW(HKEY_CURRENT_USER, kViewRoot);
  LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kViewRoot, 0, nullptr,
                            REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root,
                            &disp);
  if (rc != ERROR_SUCCESS) {
    MONO_LOG("RegCreateKeyEx(view root) failed %ld", rc);
    if (err) *err = "cannot create the engine's registry view";
    return false;
  }
  rc = RegCreateKeyExA(root, kFontRegPath, 0, nullptr, REG_OPTION_VOLATILE,
                       KEY_ALL_ACCESS, nullptr, &fonts, &disp);
  if (rc != ERROR_SUCCESS) {
    RegCloseKey(root);
    MONO_LOG("RegCreateKeyEx(SpeechFonts) failed %ld", rc);
    if (err) *err = "cannot create the engine's font list";
    return false;
  }
  RegSetValueExA(fonts, "Path", 0, REG_SZ, (const BYTE *)sf_dir_.c_str(),
                 (DWORD)sf_dir_.size() + 1);

  int published = 0;
  for (int i = 0; i < kFontCount; i++) {
    if (!font_available(i)) continue;
    HKEY k = nullptr;
    if (RegCreateKeyExA(fonts, kFonts[i].name, 0, nullptr, REG_OPTION_VOLATILE,
                        KEY_ALL_ACCESS, nullptr, &k, &disp) != ERROR_SUCCESS)
      continue;
    // Mandatory: the engine reads this into an uninitialised buffer.
    RegSetValueExA(k, "Description", 0, REG_SZ,
                   (const BYTE *)kFonts[i].display,
                   (DWORD)strlen(kFonts[i].display) + 1);
    RegCloseKey(k);
    published++;
  }
  RegCloseKey(fonts);

  LONG orc = RegOverridePredefKey(HKEY_LOCAL_MACHINE, root);
  void *scb = nullptr;
  if (orc == ERROR_SUCCESS) {
    scb = api_->OpenSpeech(hwnd_, nullptr, name);
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  } else {
    MONO_LOG("RegOverridePredefKey failed %ld", orc);
  }
  RegCloseKey(root);

  MONO_LOG("OpenSpeech(\"%s\") -> %p (%d fonts published)", name, scb,
           published);
  if (!scb) {
    if (err) *err = std::string("the engine could not open the voice '") +
                    name + "'";
    return false;
  }
  *out_scb = scb;
  return true;
}

bool Engine::select_font(const char *font_name, std::string *err) {
  int idx = font_index(font_name);
  if (idx < 0) {
    MONO_LOG("unknown font '%s'", font_name);
    if (err) *err = std::string("unknown voice '") + font_name + "'";
    return false;
  }
  if (!scb_[idx]) {
    void *scb = nullptr;
    if (!open_font(kFonts[idx].name, &scb, err)) return false;
    scb_[idx] = scb;
    int rate = 0;
    if (api_->GetSpeechParameter(scb, PARAM_SAMPLE_RATE, &rate) == 0 &&
        rate > 0)
      sample_rate_ = rate;
  }
  active_ = scb_[idx];
  current_font_ = kFonts[idx].name;
  return true;
}

void Engine::apply_params(void *scb, const Params &p) {
  for (int i = 0; i < kParamCount; i++) {
    int v = clamp_param(i, p.v[i]);
    int rc = api_->SetSpeechParameter(scb, kParams[i].id, v);
    if (rc != 0)
      MONO_LOG("SetSpeechParameter(%s=%d) rc=%d", kParams[i].name, v, rc);
  }
}

bool Engine::render(const std::string &text, const Params &p, PcmSink sink,
                    void *user, std::string *err) {
  if (!active_) {
    if (err) *err = "no voice selected";
    return false;
  }
  if (text.empty()) return true;

  apply_params(active_, p);

  void *cmd = api_->TextToCmd(active_, text.c_str(), 0);
  if (!cmd) {
    MONO_LOG("TextToCmd returned null for %u bytes", (unsigned)text.size());
    if (err) *err = "the engine could not process the text";
    return false;
  }

  int rc = api_->OpenBackendCmd(active_, cmd);
  if (rc != 0) {
    MONO_LOG("OpenBackendCmd rc=%d", rc);
    api_->FreePhoneticsCommandStream(cmd);
    if (err) *err = "the engine's render backend refused to start";
    return false;
  }

  // Pull in small blocks: it keeps first-audio latency low and makes a stop
  // request take effect within one block rather than one utterance.
  const int kBlockSamples = 1024;
  static int16_t buf[kBlockSamples];
  bool cancelled = false;
  long total = 0;
  for (;;) {
    int got = api_->GetPCMdata(active_, buf, (int)sizeof buf);
    if (got <= 0) break;
    size_t samples = (size_t)got / sizeof(int16_t);
    total += (long)samples;
    if (sink && !sink(buf, samples, user)) {
      cancelled = true;
      break;
    }
    // A short block is the engine telling us the utterance is finished.
    if (got < (int)sizeof buf) break;
  }

  api_->CloseBackend(active_);
  api_->FreePhoneticsCommandStream(cmd);
  MONO_LOG("rendered %u chars -> %ld samples%s", (unsigned)text.size(), total,
           cancelled ? " (cancelled)" : "");
  return true;
}

}  // namespace mono
