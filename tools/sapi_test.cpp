// sapi_test.cpp - end-to-end check of the Monologue '97 SAPI5 voice.
//
// Registers the dll per-user (no elevation), then drives the real SAPI stack:
// enumerates the tokens SAPI can see, speaks through each one into a wav file
// and reports how much audio came back. This exercises exactly what a screen
// reader does -- COM activation, token attributes, ISpTTSEngine::Speak, the
// pipe to the 32-bit helper and the engine itself.
//
//   sapi_test <path-to-monologue_sapi_ARCH.dll> [outdir]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sapi.h>
#include <stdio.h>

#include <string>

typedef HRESULT(STDAPICALLTYPE *PFN_DllInstall)(BOOL, LPCWSTR);

static int g_pass = 0, g_fail = 0;

static bool wav_bytes(const wchar_t *path, DWORD *out) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return false;
  *out = fad.nFileSizeLow;
  return true;
}

// FNV-1a over the sample data, to tell two renders apart.
static unsigned long long wav_hash(const wchar_t *path) {
  FILE *f = _wfopen(path, L"rb");
  if (!f) return 0;
  fseek(f, 44, SEEK_SET);
  unsigned long long h = 1469598103934665603ULL;
  unsigned char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    for (size_t i = 0; i < n; i++) {
      h ^= buf[i];
      h *= 1099511628211ULL;
    }
  fclose(f);
  return h;
}

// Peak amplitude of a 16-bit PCM wav, so silence is reported as a failure.
static int wav_peak(const wchar_t *path) {
  FILE *f = _wfopen(path, L"rb");
  if (!f) return -1;
  fseek(f, 44, SEEK_SET);
  int peak = 0;
  short buf[4096];
  size_t n;
  while ((n = fread(buf, sizeof(short), 4096, f)) > 0)
    for (size_t i = 0; i < n; i++) {
      int v = buf[i] < 0 ? -(int)buf[i] : (int)buf[i];
      if (v > peak) peak = v;
    }
  fclose(f);
  return peak;
}

static void speak_token(ISpObjectToken *token, const std::wstring &name,
                        const std::wstring &outdir, int index) {
  ISpVoice *voice = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                              IID_ISpVoice, (void **)&voice))) {
    printf("  FAIL %-34ls cannot create SpVoice\n", name.c_str());
    g_fail++;
    return;
  }
  HRESULT hr = voice->SetVoice(token);
  if (FAILED(hr)) {
    printf("  FAIL %-34ls SetVoice 0x%08lX\n", name.c_str(), hr);
    voice->Release();
    g_fail++;
    return;
  }

  wchar_t path[MAX_PATH];
  _snwprintf_s(path, _TRUNCATE, L"%s\\sapi-%02d.wav", outdir.c_str(), index);
  DeleteFileW(path);

  // Built by hand rather than with CSpStreamFormat: sphelper.h does not
  // compile cleanly against current Windows SDKs.
  WAVEFORMATEX wfx;
  memset(&wfx, 0, sizeof wfx);
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 1;
  wfx.nSamplesPerSec = 11025;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = 2;
  wfx.nAvgBytesPerSec = 11025 * 2;

  ISpStream *stream = nullptr;
  hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                        (void **)&stream);
  if (SUCCEEDED(hr))
    hr = stream->BindToFile(path, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx,
                            &wfx, SPFEI_ALL_EVENTS);
  if (FAILED(hr)) {
    printf("  FAIL %-34ls BindToFile 0x%08lX\n", name.c_str(), hr);
    if (stream) stream->Release();
    voice->Release();
    g_fail++;
    return;
  }
  voice->SetOutput(stream, TRUE);

  const wchar_t *text =
      L"Monologue ninety seven, speaking through the speech A P I.";
  ULONGLONG t0 = GetTickCount64();
  hr = voice->Speak(text, SPF_DEFAULT, nullptr);
  ULONGLONG ms = GetTickCount64() - t0;
  stream->Close();
  stream->Release();
  voice->Release();

  DWORD bytes = 0;
  wav_bytes(path, &bytes);
  int peak = wav_peak(path);
  if (FAILED(hr) || bytes <= 64 || peak < 200) {
    printf("  FAIL %-34ls hr=0x%08lX %lu bytes peak %d\n", name.c_str(), hr,
           bytes, peak);
    g_fail++;
  } else {
    printf("  ok   %-34ls %6.2fs peak %5d  (%llu ms)\n", name.c_str(),
           (bytes - 44) / 2.0 / 11025.0, peak, ms);
    g_pass++;
  }
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    printf("usage: sapi_test <monologue_sapi_ARCH.dll> [outdir]\n");
    return 1;
  }
  const std::wstring dllpath = argv[1];
  const std::wstring outdir = argc > 2 ? argv[2] : L".";
  CreateDirectoryW(outdir.c_str(), nullptr);

  HMODULE m = LoadLibraryW(dllpath.c_str());
  if (!m) {
    printf("LoadLibrary failed: %lu\n", GetLastError());
    return 2;
  }
  PFN_DllInstall inst = (PFN_DllInstall)GetProcAddress(m, "DllInstall");
  if (!inst) {
    printf("DllInstall not exported\n");
    return 3;
  }
  HRESULT hr = inst(TRUE, L"user");
  printf("per-user registration: 0x%08lX\n", hr);
  if (FAILED(hr)) return 4;

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 5;

  ISpObjectTokenCategory *cat = nullptr;
  IEnumSpObjectTokens *en = nullptr;
  hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                        IID_ISpObjectTokenCategory, (void **)&cat);
  if (SUCCEEDED(hr)) hr = cat->SetId(SPCAT_VOICES, FALSE);
  if (SUCCEEDED(hr)) hr = cat->EnumTokens(nullptr, nullptr, &en);
  if (FAILED(hr)) {
    printf("cannot enumerate voices: 0x%08lX\n", hr);
    return 6;
  }

  ULONG count = 0;
  hr = en->GetCount(&count);
  printf("SAPI enumerates %lu voices (category is HKLM-rooted)\n", count);

  int index = 0;
  int found_enumerated = 0;
  for (ULONG i = 0; i < count; i++) {
    ISpObjectToken *tok = nullptr;
    if (FAILED(en->Item(i, &tok)) || !tok) continue;
    WCHAR *id = nullptr;
    tok->GetId(&id);
    std::wstring sid = id ? id : L"";
    if (id) CoTaskMemFree(id);
    if (sid.find(L"Monologue97_") == std::wstring::npos) {
      tok->Release();
      continue;
    }
    found_enumerated++;
    WCHAR *desc = nullptr;
    tok->GetStringValue(nullptr, &desc);
    std::wstring name = desc ? desc : sid;
    if (desc) CoTaskMemFree(desc);
    speak_token(tok, name, outdir, index++);
    tok->Release();
  }
  en->Release();
  if (cat) cat->Release();

  // SPCAT_VOICES resolves to an HKEY_LOCAL_MACHINE path, so a per-user
  // registration is never enumerated -- but a token can still be bound by its
  // explicit id. That exercises the whole chain (token attributes ->
  // SetObjectToken -> Speak -> helper -> engine) without needing elevation;
  // the only thing it cannot prove is that the voice shows up in a host's voice
  // list, which is purely a matter of registering into HKLM.
  if (found_enumerated == 0) {
    printf("(not in the enumerated list: registered per-user. Binding the\n"
           " HKCU tokens by explicit id instead.)\n");
    HKEY tokens = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens", 0,
                      KEY_ENUMERATE_SUB_KEYS, &tokens) == ERROR_SUCCESS) {
      wchar_t sub[256];
      DWORD n = 0, len = 256;
      while (RegEnumKeyExW(tokens, n++, sub, &len, nullptr, nullptr, nullptr,
                           nullptr) == ERROR_SUCCESS) {
        len = 256;
        if (wcsncmp(sub, L"Monologue97_", 12) != 0) continue;
        std::wstring full =
            std::wstring(
                L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\Voices\\"
                L"Tokens\\") +
            sub;
        ISpObjectToken *tok = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                    IID_ISpObjectToken, (void **)&tok)))
          continue;
        if (SUCCEEDED(tok->SetId(nullptr, full.c_str(), FALSE))) {
          WCHAR *desc = nullptr;
          tok->GetStringValue(nullptr, &desc);
          std::wstring name = desc ? desc : sub;
          if (desc) CoTaskMemFree(desc);
          speak_token(tok, name, outdir, index++);
        }
        tok->Release();
      }
      RegCloseKey(tokens);
    }
  }
  CoUninitialize();

  // Producing audio is not enough: if the token's speech font never reaches
  // the engine, every voice renders identically and each one still "passes".
  // Compare the renders and fail if distinct voices came back the same.
  printf("\n--- distinctness ---\n");
  int distinct = 0, dup = 0;
  for (int i = 0; i < index; i++) {
    wchar_t a[MAX_PATH];
    _snwprintf_s(a, _TRUNCATE, L"%s\\sapi-%02d.wav", outdir.c_str(), i);
    unsigned long long ha = wav_hash(a);
    bool seen = false;
    for (int j = 0; j < i; j++) {
      wchar_t b[MAX_PATH];
      _snwprintf_s(b, _TRUNCATE, L"%s\\sapi-%02d.wav", outdir.c_str(), j);
      if (wav_hash(b) == ha) {
        printf("  DUPLICATE: voice %d renders identically to voice %d\n", i, j);
        seen = true;
        break;
      }
    }
    if (seen)
      dup++;
    else
      distinct++;
  }
  printf("  %d distinct renders out of %d voices\n", distinct, index);
  if (dup > 1) {
    printf("  FAIL: the speech font is not reaching the engine\n");
    g_fail += dup;
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 7 : 0;
}
