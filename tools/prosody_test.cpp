// prosody_test.cpp - does the host's pitch, rate and volume actually do
// anything?
//
// SAPI has no ISpTTSEngineSite::GetPitch. Pitch arrives *only* as
// SPVTEXTFRAG::State.PitchAdj.MiddleAdj, set from a <pitch absmiddle="N"> tag --
// which is what NVDA emits. An engine that reads GetRate and GetVolume but never
// looks at the fragment state will move rate and volume correctly and ignore
// pitch entirely, with nothing to indicate anything is wrong.
//
// This measures the rendered audio rather than trusting the call: fundamental
// frequency for pitch, duration for rate, peak level for volume.
//
//   prosody_test <monologue_sapi_ARCH.dll> [outdir]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sapi.h>
#include <stdio.h>
#include <math.h>

#include <string>
#include <vector>

typedef HRESULT(STDAPICALLTYPE *PFN_DllInstall)(BOOL, LPCWSTR);

static int g_fail = 0;

// Read the PCM body, walking the RIFF chunks: SAPI writes an 18-byte fmt
// chunk, so the header is 46 bytes and "skip 44" would misread it.
static bool read_wav(const wchar_t *path, std::vector<short> *out) {
  FILE *f = _wfopen(path, L"rb");
  if (!f) return false;
  char riff[12];
  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) ||
      memcmp(riff + 8, "WAVE", 4)) {
    fclose(f);
    return false;
  }
  long data_len = -1;
  for (;;) {
    char id[4];
    unsigned int sz = 0;
    if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
    if (!memcmp(id, "data", 4)) { data_len = (long)sz; break; }
    fseek(f, (long)sz + (sz & 1), SEEK_CUR);
  }
  if (data_len <= 0) { fclose(f); return false; }
  out->resize((size_t)data_len / sizeof(short));
  fread(out->data(), 1, (size_t)data_len, f);
  fclose(f);
  return true;
}

static int peak_of(const std::vector<short> &p) {
  int pk = 0;
  for (size_t i = 0; i < p.size(); i++) {
    int v = p[i] < 0 ? -(int)p[i] : (int)p[i];
    if (v > pk) pk = v;
  }
  return pk;
}

// Fundamental frequency by autocorrelation over the loudest voiced window.
static double f0_of(const std::vector<short> &p, int rate) {
  const size_t win = 2048;
  if (p.size() < win) return 0;
  size_t best = 0;
  double best_energy = -1;
  for (size_t i = 0; i + win < p.size(); i += 512) {
    double e = 0;
    for (size_t j = 0; j < win; j++) e += fabs((double)p[i + j]);
    if (e > best_energy) { best_energy = e; best = i; }
  }
  std::vector<double> seg(win);
  double mean = 0;
  for (size_t j = 0; j < win; j++) mean += p[best + j];
  mean /= (double)win;
  for (size_t j = 0; j < win; j++) seg[j] = p[best + j] - mean;

  const int lo = rate / 400, hi = rate / 60;  // 60..400 Hz
  double bestval = 0;
  int bestlag = 0;
  for (int lag = lo; lag < hi && lag < (int)win; lag++) {
    double s = 0;
    for (size_t j = 0; j + lag < win; j++) s += seg[j] * seg[j + lag];
    if (s > bestval) { bestval = s; bestlag = lag; }
  }
  return bestlag ? (double)rate / bestlag : 0;
}

static bool speak_xml(ISpVoice *voice, const std::wstring &xml,
                      const std::wstring &path, std::vector<short> *pcm) {
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
                                &SPDFID_WaveFormatEx, &wfx,
                                SPFEI_ALL_EVENTS))) {
    if (stream) stream->Release();
    return false;
  }
  voice->SetOutput(stream, TRUE);
  HRESULT hr = voice->Speak(xml.c_str(), SPF_IS_XML, nullptr);
  stream->Close();
  stream->Release();
  return SUCCEEDED(hr) && read_wav(path.c_str(), pcm);
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    printf("usage: prosody_test <monologue_sapi_ARCH.dll> [outdir]\n");
    return 1;
  }
  const std::wstring outdir = argc > 2 ? argv[2] : L".";
  CreateDirectoryW(outdir.c_str(), nullptr);

  HMODULE m = LoadLibraryW(argv[1]);
  if (!m) { printf("cannot load the dll: %lu\n", GetLastError()); return 2; }
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
                        L"Voices\\Tokens\\Monologue97_ENMH", FALSE)) ||
      FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              (void **)&voice)) ||
      FAILED(voice->SetVoice(tok))) {
    printf("cannot bind the voice\n");
    return 4;
  }

  const wchar_t *text = L"The quick brown fox jumps over the lazy dog";
  const int levels[] = {-10, -5, 0, 5, 10};
  const int N = 5;

  // --- pitch, via <pitch absmiddle>, which is what NVDA sends -------------
  //
  // Swept one step at a time across SAPI's whole -10..+10 range, because the
  // engine's own Pitch parameter is coarse and flattens off at both ends; a
  // five-point check cannot tell "the control works but saturates" apart from
  // "the control does nothing".
  printf("--- pitch (<pitch absmiddle=N>), measured as F0 ---\n");
  double pf0[21];
  int distinct = 0;
  bool monotonic = true;
  for (int a = -10; a <= 10; a++) {
    const int i = a + 10;
    wchar_t xml[256];
    _snwprintf_s(xml, _TRUNCATE, L"<pitch absmiddle=\"%d\">%s</pitch>", a,
                 text);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\pitch%+03d.wav", outdir.c_str(), a);
    std::vector<short> pcm;
    if (!speak_xml(voice, xml, path, &pcm)) {
      printf("  FAIL pitch %+d: nothing rendered\n", a);
      g_fail++;
      pf0[i] = 0;
      continue;
    }
    pf0[i] = f0_of(pcm, 11025);
    printf("  pitch %+3d -> F0 %6.1f Hz\n", a, pf0[i]);
    if (i && pf0[i] < pf0[i - 1] - 1.0) monotonic = false;
    if (!i || pf0[i] > pf0[i - 1] + 1.0) distinct++;
  }
  {
    const double spread = (pf0[0] > 0) ? pf0[20] / pf0[0] : 0;
    if (!monotonic || spread < 1.5 || distinct < 3) {
      printf("  FAIL pitch does not follow the host: F0 %.1f -> %.1f Hz "
             "(x%.2f), %d distinct levels%s\n",
             pf0[0], pf0[20], spread, distinct,
             monotonic ? "" : ", and it is not monotonic");
      g_fail++;
    } else {
      printf("  ok   pitch tracks the host: F0 %.1f -> %.1f Hz (x%.2f) over "
             "%d distinct levels\n",
             pf0[0], pf0[20], spread, distinct);
    }
  }

  // --- rate, which must keep working -------------------------------------
  printf("\n--- rate (<rate absspeed=N>), measured as duration ---\n");
  double dur[N];
  for (int i = 0; i < N; i++) {
    wchar_t xml[256];
    _snwprintf_s(xml, _TRUNCATE, L"<rate absspeed=\"%d\">%s</rate>", levels[i],
                 text);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\rate%+03d.wav", outdir.c_str(),
                 levels[i]);
    std::vector<short> pcm;
    if (!speak_xml(voice, xml, path, &pcm)) {
      printf("  FAIL rate %+d: nothing rendered\n", levels[i]);
      g_fail++;
      dur[i] = 0;
      continue;
    }
    dur[i] = pcm.size() / 11025.0;
    printf("  rate  %+3d -> %.2fs\n", levels[i], dur[i]);
  }
  {
    bool falling = true;
    for (int i = 1; i < N; i++)
      if (dur[i] >= dur[i - 1] - 0.01) falling = false;
    if (!falling) {
      printf("  FAIL rate does not follow the host: %.2fs -> %.2fs\n", dur[0],
             dur[N - 1]);
      g_fail++;
    } else {
      printf("  ok   rate tracks the host: %.2fs -> %.2fs\n", dur[0],
             dur[N - 1]);
    }
  }

  // --- volume, which must keep working -----------------------------------
  printf("\n--- volume (<volume level=N>), measured as peak ---\n");
  const int vols[] = {10, 50, 100};
  int pk[3];
  for (int i = 0; i < 3; i++) {
    wchar_t xml[256];
    _snwprintf_s(xml, _TRUNCATE, L"<volume level=\"%d\">%s</volume>", vols[i],
                 text);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\vol%03d.wav", outdir.c_str(), vols[i]);
    std::vector<short> pcm;
    if (!speak_xml(voice, xml, path, &pcm)) {
      printf("  FAIL volume %d: nothing rendered\n", vols[i]);
      g_fail++;
      pk[i] = 0;
      continue;
    }
    pk[i] = peak_of(pcm);
    printf("  vol   %3d -> peak %5d\n", vols[i], pk[i]);
  }
  if (!(pk[0] < pk[1] && pk[1] <= pk[2])) {
    printf("  FAIL volume does not follow the host: %d / %d / %d\n", pk[0],
           pk[1], pk[2]);
    g_fail++;
  } else {
    printf("  ok   volume tracks the host: %d -> %d\n", pk[0], pk[2]);
  }

  printf("\n%s\n", g_fail ? "FAILED" : "all prosody controls work");
  voice->Release();
  tok->Release();
  inst(FALSE, L"user");
  CoUninitialize();
  return g_fail ? 5 : 0;
}
