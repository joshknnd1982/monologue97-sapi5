// mono_sapi.cpp - the SAPI5 ISpTTSEngine implementation, plus the COM plumbing
// and voice-token registration. Built for both x86 and x64; both talk to the
// 32-bit helper.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <new>
#include <string>
#include <vector>

#include "mono_client.h"
#include "mono_log.h"
#include "mono_settings.h"
#include "mono_voices.h"

// {8F2B71A4-3C6D-4E19-9B85-7C4A2D6F1E30} - the TTS engine class.
static const CLSID CLSID_MonologueEngine = {
    0x8f2b71a4, 0x3c6d, 0x4e19,
    {0x9b, 0x85, 0x7c, 0x4a, 0x2d, 0x6f, 0x1e, 0x30}};

static const wchar_t *kClsidString = L"{8F2B71A4-3C6D-4E19-9B85-7C4A2D6F1E30}";
static const wchar_t *kVoicesPath =
    L"Software\\Microsoft\\Speech\\Voices\\Tokens";
static const wchar_t *kCustomTokenId = L"Monologue97_Custom";
static const wchar_t *kCustomTokenName = L"Monologue 97 Custom Voice";

static HINSTANCE g_dll = nullptr;
static LONG g_objects = 0;
static LONG g_locks = 0;

namespace {

// Output stays at the engine's own 11025 Hz. Offering SAPI 22050 or 44100 and
// upsampling here was measured against WASAPI loopback in case 11025 was an
// awkward rate for a 48 kHz endpoint: it made no difference (88-94 ms mean and
// 95-112 ms worst either way, across repeated runs), so the resampler was not
// worth shipping.
// --- speaking single characters -------------------------------------------
//
// Twenty printable characters render as *pure silence* in this engine --
// space ! " ' ( ) , - . / : ; ? [ \ ] ^ { | } -- so arrowing onto one of them
// says nothing at all. Every ASCII symbol therefore gets an explicit spoken
// name, which also makes the announcement the same whichever symbol level the
// screen reader is set to.
const char *symbol_name(char c) {
  switch (c) {
    case ' ':  return "space";
    case '\t': return "tab";
    case '\r':
    case '\n': return "new line";
    case '!':  return "exclamation";
    case '"':  return "quote";
    case '#':  return "number";
    case '$':  return "dollar";
    case '%':  return "percent";
    case '&':  return "and";
    case '\'': return "apostrophe";
    case '(':  return "left paren";
    case ')':  return "right paren";
    case '*':  return "star";
    case '+':  return "plus";
    case ',':  return "comma";
    case '-':  return "dash";
    case '.':  return "dot";
    case '/':  return "slash";
    case ':':  return "colon";
    case ';':  return "semicolon";
    case '<':  return "less than";
    case '=':  return "equals";
    case '>':  return "greater than";
    case '?':  return "question";
    case '@':  return "at";
    case '[':  return "left bracket";
    case '\\': return "backslash";
    case ']':  return "right bracket";
    case '^':  return "caret";
    case '_':  return "underline";
    case '`':  return "backtick";
    case '{':  return "left brace";
    case '|':  return "bar";
    case '}':  return "right brace";
    case '~':  return "tilde";
    default:   return nullptr;  // letters and digits speak for themselves
  }
}

// SPVA_SpellOut: say the text one character at a time. NVDA wraps character
// navigation in <spell>...</spell>, which SAPI delivers as this action.
std::string spell_out(const std::string &text) {
  std::string out;
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (!out.empty()) out += ' ';
    const char *nm = symbol_name(c);
    if (nm)
      out += nm;
    else
      out += c;
  }
  return out;
}

// Indices into mono::kParams, which is ordered Speed, Pitch, Volume, ...
const int kSpeedIdx = 0;
const int kPitchIdx = 1;

// Shift one engine parameter by a SAPI adjustment. SAPI expresses rate and
// pitch on a -10..+10 scale, which is spread across the parameter's own range
// so that the extremes of the host's slider reach the extremes of the engine.
int adjust(int idx, int base, long sapi_adj) {
  if (!sapi_adj) return base;
  const int lo = mono::kParams[idx].lo, hi = mono::kParams[idx].hi;
  if (sapi_adj < -10) sapi_adj = -10;
  if (sapi_adj > 10) sapi_adj = 10;
  // Rounded, not truncated: the engine's ranges are small enough that
  // truncation would waste steps and leave part of the host's slider inert.
  const double step = (double)sapi_adj * (hi - lo) / 20.0;
  const int delta = (int)(step < 0 ? step - 0.5 : step + 0.5);
  return mono::clamp_param(idx, base + delta);
}

std::string narrow_ansi(const std::wstring &w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------

class MonologueEngine : public ISpTTSEngine, public ISpObjectWithToken {
 public:
  MonologueEngine() : ref_(1) { InterlockedIncrement(&g_objects); }
  virtual ~MonologueEngine() { InterlockedDecrement(&g_objects); }

  // --- IUnknown ---
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(ISpTTSEngine))
      *ppv = static_cast<ISpTTSEngine *>(this);
    else if (riid == __uuidof(ISpObjectWithToken))
      *ppv = static_cast<ISpObjectWithToken *>(this);
    else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    LONG n = InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return n;
  }

  // --- ISpObjectWithToken ---
  STDMETHODIMP SetObjectToken(ISpObjectToken *token) override {
    if (!token) return E_INVALIDARG;
    token_ = token;
    token_->AddRef();

    // MonoFont/MonoCustom live in the token's Attributes subkey, alongside
    // Gender and Language, so they have to be read through that key --
    // token->GetStringValue would look in the token key itself and silently
    // return nothing, leaving every voice on the default font.
    ISpDataKey *attrs = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
      WCHAR *val = nullptr;
      if (SUCCEEDED(attrs->GetStringValue(L"MonoFont", &val)) && val) {
        strncpy_s(font_, sizeof font_, narrow_ansi(val).c_str(), _TRUNCATE);
        CoTaskMemFree(val);
      }
      val = nullptr;
      if (SUCCEEDED(attrs->GetStringValue(L"MonoCustom", &val)) && val) {
        custom_ = (val[0] == L'1');
        CoTaskMemFree(val);
      }
      attrs->Release();
    } else {
      MONO_LOG("SetObjectToken: no Attributes key on this token");
    }
    MONO_LOG("SetObjectToken: font=%s custom=%d", font_, (int)custom_);
    return S_OK;
  }

  STDMETHODIMP GetObjectToken(ISpObjectToken **ppToken) override {
    if (!ppToken) return E_POINTER;
    *ppToken = token_;
    if (token_) token_->AddRef();
    return token_ ? S_OK : E_UNEXPECTED;
  }

  // --- ISpTTSEngine ---
  STDMETHODIMP GetOutputFormat(const GUID *pTargetFmtId,
                               const WAVEFORMATEX *pTarget,
                               GUID *pOutputFormatId,
                               WAVEFORMATEX **ppCoMemOutput) override {
    if (!pOutputFormatId || !ppCoMemOutput) return E_POINTER;
    if (pTarget)
      MONO_LOG("GetOutputFormat: host wants %luHz %ubit %uch (tag %u)",
               pTarget->nSamplesPerSec, pTarget->wBitsPerSample,
               pTarget->nChannels, pTarget->wFormatTag);
    else
      MONO_LOG("GetOutputFormat: host expressed no preference");

    WAVEFORMATEX *wfx = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!wfx) return E_OUTOFMEMORY;
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = mono::kChannels;
    wfx->nSamplesPerSec = mono::kSampleRate;
    wfx->wBitsPerSample = mono::kBitsPerSample;
    wfx->nBlockAlign = (WORD)(wfx->nChannels * wfx->wBitsPerSample / 8);
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = 0;
    *ppCoMemOutput = wfx;
    *pOutputFormatId = SPDFID_WaveFormatEx;
    return S_OK;
  }

  STDMETHODIMP Speak(DWORD, REFGUID, const WAVEFORMATEX *,
                     const SPVTEXTFRAG *pFrags,
                     ISpTTSEngineSite *pSite) override;

 private:
  struct SinkState {
    ISpTTSEngineSite *site;
    double gain;
    bool abort;
    double first_block;  // <0 until the first block is handed to SAPI
    size_t samples;      // total handed over, for the timing log
  };

  static bool sink(const int16_t *pcm, size_t count, void *user);

  LONG ref_;
  ISpObjectToken *token_ = nullptr;
  char font_[16] = "ENMH";
  bool custom_ = false;
  int out_rate_ = mono::kSampleRate;
  mono::Client client_;
};

bool MonologueEngine::sink(const int16_t *pcm, size_t count, void *user) {
  SinkState *st = (SinkState *)user;

  // SAPI can ask us to stop between any two blocks; honouring it here is what
  // makes a screen reader feel responsive when the user keeps arrowing.
  if (st->site->GetActions() & SPVES_ABORT) {
    st->abort = true;
    return false;
  }

  if (st->first_block < 0) st->first_block = 0;  // marked by the caller's clock
  st->samples += count;

  const int16_t *out = pcm;
  std::vector<int16_t> scaled;
  if (st->gain < 0.999) {
    // SAPI volume as software gain. The engine's own Volume parameter is
    // coarse (ten steps, and the top half just clips), so the host's 0..100
    // slider is applied here instead, where it is smooth and never distorts.
    scaled.assign(pcm, pcm + count);
    for (size_t i = 0; i < scaled.size(); i++)
      scaled[i] = (int16_t)(scaled[i] * st->gain);
    out = scaled.data();
  }

  ULONG written = 0;
  HRESULT hr = st->site->Write(out, (ULONG)(count * sizeof(int16_t)), &written);
  return SUCCEEDED(hr);
}

STDMETHODIMP MonologueEngine::Speak(DWORD, REFGUID, const WAVEFORMATEX *,
                                    const SPVTEXTFRAG *pFrags,
                                    ISpTTSEngineSite *pSite) {
  if (!pSite) return E_POINTER;

  // A Custom Voice token re-reads the configuration utility's snapshot for
  // every utterance, so changes take effect immediately rather than at the
  // next restart of the host application.
  mono::Params params;
  const char *font = font_;
  mono::Settings settings;
  if (custom_) {
    settings = mono::load_settings();
    params = mono::settings_to_params(settings);
    font = settings.font;
  } else {
    params = mono::default_params();
  }

  // The voice-level base settings. Rate and volume have one of these; pitch
  // does not -- there is no ISpTTSEngineSite::GetPitch, and SAPI delivers pitch
  // *only* per fragment as State.PitchAdj.MiddleAdj (set by <pitch absmiddle>,
  // which is exactly how NVDA adjusts pitch). Reading only GetRate/GetVolume
  // therefore moves rate and volume correctly and ignores pitch completely.
  long base_rate = 0;
  USHORT base_volume = 100;
  pSite->GetRate(&base_rate);
  pSite->GetVolume(&base_volume);

  SinkState st;
  st.site = pSite;
  st.gain = 1.0;  // set per fragment below
  st.abort = false;
  st.first_block = -1;
  st.samples = 0;

  // Timed with QPC, not GetTickCount, whose 15.6 ms tick would report a flat
  // "15 ms" for everything interesting here. This is what to read out of the
  // log if the voice still feels slow in a real screen reader: it separates
  // our own cost from whatever the host's audio stack adds after us.
  LARGE_INTEGER qpf, t0;
  QueryPerformanceFrequency(&qpf);
  QueryPerformanceCounter(&t0);

  std::string err;
  for (const SPVTEXTFRAG *f = pFrags; f; f = f->pNext) {
    if (pSite->GetActions() & SPVES_ABORT) break;

    // Bookmarks and other non-speech fragments carry text too, and speaking
    // those makes the engine read a screen reader's internal bookmark numbers
    // out loud -- but SPVA_SpellOut *is* speech. NVDA wraps character-by-
    // character navigation in <spell>...</spell>, so treating anything that is
    // not SPVA_Speak as non-speech silently drops every letter and every
    // punctuation mark the user arrows over.
    const bool spell = (f->State.eAction == SPVA_SpellOut);
    if (f->State.eAction != SPVA_Speak && !spell) continue;
    if (!f->pTextStart || f->ulTextLen == 0) continue;

    std::wstring w(f->pTextStart, f->ulTextLen);
    std::string ansi = narrow_ansi(w);

    if (spell) {
      ansi = spell_out(ansi);
    } else if (ansi.size() == 1 && symbol_name(ansi[0])) {
      // A fragment that is a single symbol only ever reaches us because
      // something is reading one character out, so name it -- the engine
      // renders it as silence otherwise. This has to come *before* the
      // whitespace test below, or arrowing onto a space says nothing.
      ansi = symbol_name(ansi[0]);
    } else if (ansi.find_first_not_of(" \t\r\n") == std::string::npos) {
      // Whitespace between words is a gap, not something to announce.
      continue;
    }
    if (ansi.empty()) continue;

    // Rate, pitch and volume are all per-fragment: SAPI combines the voice's
    // base setting with any <rate>/<pitch>/<volume> tag and reports the result
    // in this fragment's state, so they are applied here rather than once per
    // utterance.
    mono::Params fp = params;
    fp.v[kSpeedIdx] =
        adjust(kSpeedIdx, fp.v[kSpeedIdx], base_rate + f->State.RateAdj);
    fp.v[kPitchIdx] =
        adjust(kPitchIdx, fp.v[kPitchIdx], f->State.PitchAdj.MiddleAdj);

    // Volume is applied as software gain rather than through the engine's own
    // Volume parameter, which has only ten steps and clips across the top half.
    double vol = (base_volume >= 100 ? 100.0 : (double)base_volume) / 100.0;
    if (f->State.Volume <= 100) vol *= f->State.Volume / 100.0;
    st.gain = vol;

    if (!client_.speak(font, fp, ansi, &MonologueEngine::sink, &st, &err)) {
      MONO_LOG("speak failed: %s", err.c_str());
      return SPERR_ENGINE_BUSY;
    }
    if (st.abort) break;
  }

  LARGE_INTEGER t1;
  QueryPerformanceCounter(&t1);
  const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                    (double)qpf.QuadPart;
  MONO_LOG("Speak: %u samples (%.0f ms of audio) handed to SAPI in %.2f ms%s",
           (unsigned)st.samples, st.samples * 1000.0 / mono::kSampleRate, ms,
           st.abort ? " (aborted)" : "");
  return S_OK;
}

// ---------------------------------------------------------------------------
// Class factory

class Factory : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid,
                              void **ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    MonologueEngine *e = new (std::nothrow) MonologueEngine();
    if (!e) return E_OUTOFMEMORY;
    HRESULT hr = e->QueryInterface(riid, ppv);
    e->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    lock ? InterlockedIncrement(&g_locks) : InterlockedDecrement(&g_locks);
    return S_OK;
  }
};
static Factory g_factory;

// ---------------------------------------------------------------------------
// Registration

namespace {

bool set_sz(HKEY k, const wchar_t *name, const wchar_t *value) {
  return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value,
                        (DWORD)((wcslen(value) + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

std::wstring widen(const char *s) {
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
  return w;
}

// Every voice is written as a static token rather than produced by a dynamic
// enumerator: static tokens are what every SAPI5 client reads, Narrator
// included, and SAPI only consults token enumerators registered in HKLM.
void write_token(HKEY tokens, const std::wstring &id, const std::wstring &name,
                 const wchar_t *gender, const char *font, bool custom) {
  HKEY t = nullptr;
  DWORD disp = 0;
  if (RegCreateKeyExW(tokens, id.c_str(), 0, nullptr, 0,
                      KEY_CREATE_SUB_KEY | KEY_SET_VALUE, nullptr, &t,
                      &disp) != ERROR_SUCCESS)
    return;
  set_sz(t, nullptr, name.c_str());
  set_sz(t, L"CLSID", kClsidString);
  // SAPI looks the display name up under a value named for the LCID it is
  // asking about, falling back to the key's default value.
  set_sz(t, L"409", name.c_str());

  HKEY a = nullptr;
  if (RegCreateKeyExW(t, L"Attributes", 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                      &a, &disp) == ERROR_SUCCESS) {
    set_sz(a, L"Name", name.c_str());
    set_sz(a, L"Language", L"409");
    set_sz(a, L"Age", L"Adult");
    set_sz(a, L"Vendor", L"First Byte");
    if (gender) set_sz(a, L"Gender", gender);
    // Read back by SetObjectToken, so the exact speech font is recovered
    // without parsing a display name apart.
    set_sz(a, L"MonoFont", widen(font).c_str());
    if (custom) set_sz(a, L"MonoCustom", L"1");
    RegCloseKey(a);
  }
  RegCloseKey(t);
}

void remove_token(HKEY tokens, const std::wstring &id) {
  HKEY t = nullptr;
  if (RegOpenKeyExW(tokens, id.c_str(), 0, KEY_ALL_ACCESS, &t) ==
      ERROR_SUCCESS) {
    RegDeleteKeyW(t, L"Attributes");
    RegCloseKey(t);
  }
  RegDeleteKeyW(tokens, id.c_str());
}

std::wstring token_id(const char *font) {
  return std::wstring(L"Monologue97_") + widen(font);
}

}  // namespace

namespace {

// `classes` is the root the CLSID goes under (HKEY_CLASSES_ROOT for a machine
// install, HKCU\Software\Classes for a per-user one) and `voices` the root for
// the SAPI token list. Factoring this out of DllRegisterServer is what lets the
// whole registration path be exercised against HKCU without elevation -- the
// alternative is that nothing tests it until an installer runs as
// administrator, and a bug that only appears once SAPI reads a token back can
// pass every other check.
HRESULT register_into(HKEY classes, HKEY voices) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_dll, path, MAX_PATH);

  std::wstring key = std::wstring(L"CLSID\\") + kClsidString;
  HKEY k = nullptr, ips = nullptr;
  DWORD disp = 0;
  if (RegCreateKeyExW(classes, key.c_str(), 0, nullptr, 0,
                      KEY_CREATE_SUB_KEY | KEY_SET_VALUE, nullptr, &k,
                      &disp) != ERROR_SUCCESS)
    return E_ACCESSDENIED;
  set_sz(k, nullptr, L"Monologue 97 SAPI5 Engine");
  if (RegCreateKeyExW(k, L"InprocServer32", 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &ips, &disp) == ERROR_SUCCESS) {
    set_sz(ips, nullptr, path);
    set_sz(ips, L"ThreadingModel", L"Both");
    RegCloseKey(ips);
  }
  RegCloseKey(k);

  HKEY tokens = nullptr;
  if (RegCreateKeyExW(voices, kVoicesPath, 0, nullptr, 0,
                      KEY_CREATE_SUB_KEY | KEY_SET_VALUE, nullptr, &tokens,
                      &disp) != ERROR_SUCCESS)
    return E_ACCESSDENIED;

  for (int i = 0; i < mono::kFontCount; i++) {
    write_token(tokens, token_id(mono::kFonts[i].name),
                widen(mono::kFonts[i].sapi),
                mono::kFonts[i].female ? L"Female" : L"Male",
                mono::kFonts[i].name, false);
  }
  // The Custom Voice: its speech font and every parameter come from whatever
  // the configuration utility last saved. No Gender attribute -- the user's
  // chosen font decides.
  write_token(tokens, kCustomTokenId, kCustomTokenName, nullptr, "ENMH", true);
  RegCloseKey(tokens);

  MONO_LOG("registered %d voice tokens + custom voice", mono::kFontCount);
  return S_OK;
}

HRESULT unregister_from(HKEY classes, HKEY voices) {
  HKEY tokens = nullptr;
  if (RegOpenKeyExW(voices, kVoicesPath, 0, KEY_ALL_ACCESS, &tokens) ==
      ERROR_SUCCESS) {
    for (int i = 0; i < mono::kFontCount; i++)
      remove_token(tokens, token_id(mono::kFonts[i].name));
    remove_token(tokens, kCustomTokenId);
    RegCloseKey(tokens);
  }
  std::wstring key = std::wstring(L"CLSID\\") + kClsidString;
  HKEY k = nullptr;
  if (RegOpenKeyExW(classes, key.c_str(), 0, KEY_ALL_ACCESS, &k) ==
      ERROR_SUCCESS) {
    RegDeleteKeyW(k, L"InprocServer32");
    RegCloseKey(k);
  }
  RegDeleteKeyW(classes, key.c_str());
  return S_OK;
}

// HKCU\Software\Classes, created if absent.
HKEY user_classes() {
  HKEY h = nullptr;
  DWORD disp = 0;
  RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0,
                  KEY_ALL_ACCESS, nullptr, &h, &disp);
  return h;
}

}  // namespace

STDAPI DllRegisterServer() {
  return register_into(HKEY_CLASSES_ROOT, HKEY_LOCAL_MACHINE);
}

STDAPI DllUnregisterServer() {
  return unregister_from(HKEY_CLASSES_ROOT, HKEY_LOCAL_MACHINE);
}

// Per-user registration: no elevation needed, and the route the test harness
// uses to drive the real SAPI stack.
STDAPI DllInstall(BOOL install, LPCWSTR cmdline) {
  const bool per_user = cmdline && wcsstr(cmdline, L"user") != nullptr;
  if (!per_user)
    return install ? DllRegisterServer() : DllUnregisterServer();
  HKEY classes = user_classes();
  if (!classes) return E_ACCESSDENIED;
  HRESULT hr = install ? register_into(classes, HKEY_CURRENT_USER)
                       : unregister_from(classes, HKEY_CURRENT_USER);
  RegCloseKey(classes);
  return hr;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
  if (rclsid != CLSID_MonologueEngine) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
  return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_dll = inst;
    DisableThreadLibraryCalls(inst);
    mono::log_init(sizeof(void *) == 8 ? "sapi-x64" : "sapi-x86");
    mono::set_module(inst);
  }
  return TRUE;
}
