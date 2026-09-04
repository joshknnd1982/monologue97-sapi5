// spell_test.cpp - arrowing character by character must actually say something.
//
// NVDA speaks character navigation by wrapping the character in
// <spell>...</spell> and passing SPF_IS_XML. SAPI turns that into a text
// fragment whose State.eAction is SPVA_SpellOut, not SPVA_Speak. An engine that
// only handles SPVA_Speak silently drops every one of them, which is invisible
// to any test that speaks plain sentences.
//
// This drives the real SAPI stack exactly the way NVDA does and fails if any
// character produces no audio.
//
//   spell_test <monologue_sapi_ARCH.dll> [outdir]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sapi.h>
#include <stdio.h>

#include <string>

typedef HRESULT(STDAPICALLTYPE *PFN_DllInstall)(BOOL, LPCWSTR);

static int g_pass = 0, g_fail = 0;

// Peak of the 16-bit PCM body of a wav, so silence is a failure.
//
// The 'data' chunk is located by walking the RIFF chunks rather than assuming
// it starts at offset 44: SAPI writes an 18-byte fmt chunk, so the header is 46
// bytes, and reading from 44 returns two header bytes as if they were audio --
// which is enough to make a silent render look like a loud one and pass.
static int wav_peak(const wchar_t *path) {
  FILE *f = _wfopen(path, L"rb");
  if (!f) return -1;
  char riff[12];
  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) ||
      memcmp(riff + 8, "WAVE", 4)) {
    fclose(f);
    return -1;
  }
  long data_len = -1;
  for (;;) {
    char id[4];
    unsigned int sz = 0;
    if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
    if (!memcmp(id, "data", 4)) {
      data_len = (long)sz;
      break;
    }
    fseek(f, (long)sz + (sz & 1), SEEK_CUR);  // chunks are word aligned
  }
  if (data_len < 0) {
    fclose(f);
    return -1;
  }
  int peak = 0;
  long left = data_len;
  short buf[4096];
  while (left > 0) {
    size_t want = (size_t)(left < (long)sizeof buf ? left : (long)sizeof buf);
    size_t got = fread(buf, 1, want, f);
    if (!got) break;
    for (size_t i = 0; i < got / sizeof(short); i++) {
      int v = buf[i] < 0 ? -(int)buf[i] : (int)buf[i];
      if (v > peak) peak = v;
    }
    left -= (long)got;
  }
  fclose(f);
  return peak;
}

static void speak_to_file(ISpVoice *voice, const std::wstring &xml,
                          const std::wstring &path, const char *label,
                          bool expect_silent = false) {
  DeleteFileW(path.c_str());
  WAVEFORMATEX wfx;
  memset(&wfx, 0, sizeof wfx);
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 1;
  wfx.nSamplesPerSec = 11025;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = 2;
  wfx.nAvgBytesPerSec = 11025 * 2;

  ISpStream *stream = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                              IID_ISpStream, (void **)&stream)) ||
      FAILED(stream->BindToFile(path.c_str(), SPFM_CREATE_ALWAYS,
                                &SPDFID_WaveFormatEx, &wfx, SPFEI_ALL_EVENTS))) {
    printf("  FAIL %-22s cannot open the output stream\n", label);
    g_fail++;
    if (stream) stream->Release();
    return;
  }
  voice->SetOutput(stream, TRUE);
  HRESULT hr = voice->Speak(xml.c_str(), SPF_IS_XML, nullptr);
  stream->Close();
  stream->Release();

  const int peak = wav_peak(path.c_str());
  if (expect_silent) {
    // SAPI strips a whitespace-only utterance itself: the engine is handed a
    // fragment with ulTextLen == 0, so there is nothing left to speak and no
    // SAPI5 voice can produce anything here. NVDA does not rely on this -- it
    // substitutes the word "space" for character navigation before speaking.
    printf("  n/a  %-22s peak=%d  (SAPI drops whitespace before the engine)\n",
           label, peak);
    return;
  }
  if (FAILED(hr) || peak < 200) {
    printf("  FAIL %-22s hr=0x%08lX peak=%d  (nothing spoken)\n", label, hr,
           peak);
    g_fail++;
  } else {
    printf("  ok   %-22s peak=%d\n", label, peak);
    g_pass++;
  }
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    printf("usage: spell_test <monologue_sapi_ARCH.dll> [outdir]\n");
    return 1;
  }
  const std::wstring outdir = argc > 2 ? argv[2] : L".";
  CreateDirectoryW(outdir.c_str(), nullptr);

  HMODULE m = LoadLibraryW(argv[1]);
  if (!m) {
    printf("cannot load the dll: %lu\n", GetLastError());
    return 2;
  }
  PFN_DllInstall inst = (PFN_DllInstall)GetProcAddress(m, "DllInstall");
  if (!inst || FAILED(inst(TRUE, L"user"))) {
    printf("per-user registration failed\n");
    return 3;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  ISpObjectToken *tok = nullptr;
  ISpVoice *voice = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                              IID_ISpObjectToken, (void **)&tok)) ||
      FAILED(tok->SetId(nullptr,
                        L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\"
                        L"Voices\\Tokens\\Monologue97_ENMH",
                        FALSE)) ||
      FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              (void **)&voice)) ||
      FAILED(voice->SetVoice(tok))) {
    printf("cannot bind the voice\n");
    return 4;
  }

  // Exactly what NVDA sends when the caret moves one character.
  printf("--- <spell> (how NVDA speaks character navigation) ---\n");
  const wchar_t *chars =
      L"abzABZ059 !\"'(),-./:;?[]^{|}#$%&*+<=>@_`~\\";
  int i = 0;
  for (const wchar_t *p = chars; *p; ++p, ++i) {
    wchar_t xml[64];
    // '&' and '<' have to survive the XML parser to reach the engine.
    const wchar_t *esc = nullptr;
    if (*p == L'&') esc = L"&amp;";
    else if (*p == L'<') esc = L"&lt;";
    else if (*p == L'>') esc = L"&gt;";
    if (esc)
      _snwprintf_s(xml, _TRUNCATE, L"<spell>%s</spell>", esc);
    else
      _snwprintf_s(xml, _TRUNCATE, L"<spell>%c</spell>", *p);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\spell-%02d.wav", outdir.c_str(), i);
    char label[32];
    _snprintf_s(label, _TRUNCATE, "spell '%lc' (U+%04X)", *p, (unsigned)*p);
    speak_to_file(voice, xml, path, label, *p == L' ');
  }

  // The same characters as ordinary one-character utterances, which is what a
  // host that does not use <spell> will send.
  printf("\n--- plain single characters ---\n");
  const wchar_t *plain = L"a. ,-?";
  i = 0;
  for (const wchar_t *p = plain; *p; ++p, ++i) {
    wchar_t xml[64];
    _snwprintf_s(xml, _TRUNCATE, L"%c", *p);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\plain-%02d.wav", outdir.c_str(), i);
    char label[32];
    _snprintf_s(label, _TRUNCATE, "plain '%lc'", *p);
    speak_to_file(voice, xml, path, label, *p == L' ');
  }

  // Spelling a whole word must produce letters, not the word.
  printf("\n--- multi-character spelling ---\n");
  speak_to_file(voice, L"<spell>cat</spell>", outdir + L"\\spell-word.wav",
                "spell \"cat\"");

  // And a normal sentence must still be a sentence.
  printf("\n--- ordinary speech still works ---\n");
  speak_to_file(voice, L"This is an ordinary sentence.",
                outdir + L"\\plain-sentence.wav", "sentence");

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  voice->Release();
  tok->Release();
  inst(FALSE, L"user");
  CoUninitialize();
  return g_fail ? 5 : 0;
}
