// render.cpp - isolate which Monologue audio path actually produces samples.
//
// Modes (argv[3]):
//   play  - Say() with no backend: engine drives waveOut itself
//   file  - OpenBackend(file) + Say()
//   pcm   - OpenBackendCmd() + Say() + GetPCMdata() pull loop
//   phon  - TextToPhonetics + SpeakPhonetics + GetPCMdata pull loop
//
// A watchdog aborts the process if a call blocks, printing where.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

typedef void *LPSCB;
typedef WORD(__stdcall *PFN_SpeechVersion)(void);
typedef LPSCB(__stdcall *PFN_OpenSpeech)(void *, void *, const char *);
typedef int(__stdcall *PFN_CloseSpeech)(LPSCB);
typedef int(__stdcall *PFN_Say)(LPSCB, const char *);
typedef int(__stdcall *PFN_SetSpeechParameter)(LPSCB, int, int);
typedef int(__stdcall *PFN_GetSpeechParameter)(LPSCB, int, int *);
typedef int(__stdcall *PFN_SpeechStatus)(LPSCB);
typedef int(__stdcall *PFN_OpenBackend)(LPSCB, const char *);
typedef int(__stdcall *PFN_OpenBackendCmd)(LPSCB, void *);
typedef int(__stdcall *PFN_CloseBackend)(LPSCB);
typedef int(__stdcall *PFN_GetPCMdata)(LPSCB, void *, int);
typedef void *(__stdcall *PFN_TextToPhonetics)(LPSCB, const char *, int);
typedef int(__stdcall *PFN_SpeakPhonetics)(LPSCB, void *);
typedef void *(__stdcall *PFN_TextToCmd)(LPSCB, const char *, int);
typedef int(__stdcall *PFN_SpeakCmd)(LPSCB, void *);
typedef int(__stdcall *PFN_ResetSpeech)(LPSCB);

static HMODULE g_dll;
#define GP(n) ((PFN_##n)GetProcAddress(g_dll, #n))

static const char *kRegPath = "Software\\FirstByte\\PrimoVOX\\SpeechFonts";

static volatile LONG g_mark = 0;
static const char *g_where = "start";
#define MARK(s)                 \
  do {                          \
    g_where = (s);              \
    InterlockedIncrement(&g_mark); \
    printf("[%s]\n", (s));      \
    fflush(stdout);             \
  } while (0)

static DWORD WINAPI watchdog(LPVOID p) {
  DWORD secs = (DWORD)(ULONG_PTR)p;
  LONG last = -1;
  for (DWORD i = 0; i < secs; i++) {
    Sleep(1000);
    LONG now = g_mark;
    if (now == last) {
      if (i > 12) {
        printf("\n[watchdog] stuck in '%s' for >12s, aborting\n", g_where);
        fflush(stdout);
        ExitProcess(99);
      }
    } else {
      last = now;
      i = 0;
    }
  }
  printf("\n[watchdog] overall timeout in '%s'\n", g_where);
  fflush(stdout);
  ExitProcess(98);
  return 0;
}

static int __stdcall fake_messagebox(HWND h, LPCSTR t, LPCSTR c, UINT u) {
  (void)h; (void)u;
  printf("[engine MessageBox] %s: %s\n", c ? c : "", t ? t : "");
  fflush(stdout);
  return IDOK;
}

static BOOL patch_import(HMODULE mod, const char *dll, const char *fn,
                         void *repl) {
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  DWORD rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (!rva) return FALSE;
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
      if (strcmp((const char *)n->Name, fn)) continue;
      DWORD old;
      VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
      ft->u1.Function = (ULONG_PTR)repl;
      VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
      return TRUE;
    }
  }
  return FALSE;
}

static void build_view(const char *sfdir, const char *font,
                       const char *desc) {
  RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\MonologueProbe");
  HKEY root, fonts, k;
  DWORD d;
  RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MonologueProbe\\HKLM", 0, NULL,
                  REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &root, &d);
  RegCreateKeyExA(root, kRegPath, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                  NULL, &fonts, &d);
  RegSetValueExA(fonts, "Path", 0, REG_SZ, (const BYTE *)sfdir,
                 (DWORD)strlen(sfdir) + 1);
  RegCreateKeyExA(fonts, font, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                  NULL, &k, &d);
  RegSetValueExA(k, "Description", 0, REG_SZ, (const BYTE *)desc,
                 (DWORD)strlen(desc) + 1);
  RegCloseKey(k);
  RegCloseKey(fonts);
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, root);
}

static void pump(void) {
  MSG m;
  while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

int main(int argc, char **argv) {
  const char *engdir = argc > 1 ? argv[1] : ".";
  const char *font = argc > 2 ? argv[2] : "ENMH";
  const char *mode = argc > 3 ? argv[3] : "play";
  char sfdir[MAX_PATH], dllpath[MAX_PATH];
  _snprintf(sfdir, sizeof sfdir, "%s\\SF", engdir);
  _snprintf(dllpath, sizeof dllpath, "%s\\mnvox11.dll", engdir);

  SetCurrentDirectoryA(engdir);
  SetDllDirectoryA(engdir);
  CreateThread(NULL, 0, watchdog, (LPVOID)(ULONG_PTR)120, 0, NULL);

  g_dll = LoadLibraryA(dllpath);
  if (!g_dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
  patch_import(g_dll, "USER32.dll", "MessageBoxA", (void *)fake_messagebox);

  PFN_OpenSpeech pOpen = GP(OpenSpeech);
  PFN_Say pSay = GP(Say);
  PFN_SpeechStatus pStatus = GP(SpeechStatus);
  PFN_OpenBackend pOpenBE = GP(OpenBackend);
  PFN_OpenBackendCmd pOpenBECmd = GP(OpenBackendCmd);
  PFN_CloseBackend pCloseBE = GP(CloseBackend);
  PFN_GetPCMdata pPCM = GP(GetPCMdata);
  PFN_TextToPhonetics pT2P = GP(TextToPhonetics);
  PFN_SpeakPhonetics pSpeakP = GP(SpeakPhonetics);
  PFN_CloseSpeech pClose = GP(CloseSpeech);
  PFN_GetSpeechParameter pGet = GP(GetSpeechParameter);

  printf("waveOutGetNumDevs = %u\n", waveOutGetNumDevs());

  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "MonologueRenderWnd";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "MonologueRenderWnd", "m", WS_OVERLAPPED, 0, 0,
                              0, 0, NULL, NULL, wc.hInstance, NULL);
  printf("hwnd=%p\n", (void *)hwnd);

  build_view(sfdir, font, "Probe Voice");
  MARK("OpenSpeech");
  LPSCB scb = pOpen(hwnd, NULL, font);
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
  printf("scb=%p\n", scb);
  if (!scb) return 2;

  int rate = 0, fmt = 0;
  pGet(scb, 20, &rate);
  pGet(scb, 21, &fmt);
  printf("engine rate=%d fmt=%d\n", rate, fmt);

  const char *text = "Monologue ninety seven speaking a test sentence.";

  if (!strcmp(mode, "play")) {
    MARK("Say (playback)");
    int rc = pSay(scb, text);
    printf("Say rc=%d\n", rc);
    for (int i = 0; i < 400; i++) {
      pump();
      MARK("poll");
      int st = pStatus(scb);
      printf("  status=%d\n", st);
      if (st == 0 && i > 3) break;
      Sleep(50);
    }
  } else if (!strcmp(mode, "file")) {
    char wav[MAX_PATH];
    _snprintf(wav, sizeof wav, "%s\\render_out.wav", engdir);
    DeleteFileA(wav);
    MARK("OpenBackend");
    printf("OpenBackend rc=%d\n", pOpenBE(scb, wav));
    MARK("Say (file)");
    printf("Say rc=%d\n", pSay(scb, text));
    MARK("CloseBackend");
    printf("CloseBackend rc=%d\n", pCloseBE(scb));
  } else if (!strcmp(mode, "pcm")) {
    MARK("OpenBackendCmd");
    printf("OpenBackendCmd rc=%d\n", pOpenBECmd(scb, NULL));
    MARK("Say (pcm)");
    printf("Say rc=%d\n", pSay(scb, text));
    MARK("pull");
    static unsigned char buf[4096];
    long total = 0;
    FILE *f = fopen("render_pcm.raw", "wb");
    for (int i = 0; i < 3000; i++) {
      pump();
      int got = pPCM(scb, buf, sizeof buf);
      if (got > 0) { fwrite(buf, 1, got, f); total += got; }
      else Sleep(2);
      if (i % 200 == 0) { MARK("pull"); printf("  total=%ld\n", total); }
    }
    fclose(f);
    printf("pcm total=%ld\n", total);
  } else if (!strcmp(mode, "cmd")) {
    // The pull path: compile the text to a command stream, hand that to the
    // "Cmd" backend, then drain synthesized PCM with GetPCMdata.
    PFN_TextToCmd pT2C = GP(TextToCmd);
    MARK("TextToCmd");
    void *cmd = pT2C(scb, text, 0);
    printf("cmd stream = %p\n", cmd);
    if (!cmd) { printf("TextToCmd failed\n"); return 3; }
    MARK("OpenBackendCmd");
    printf("OpenBackendCmd rc=%d\n", pOpenBECmd(scb, cmd));
    MARK("pull");
    static unsigned char buf[8192];
    long total = 0;
    int zero = 0;
    FILE *f = fopen("render_cmd.raw", "wb");
    for (int i = 0; i < 20000; i++) {
      int got = pPCM(scb, buf, sizeof buf);
      if (got > 0) {
        fwrite(buf, 1, got, f);
        total += got;
        zero = 0;
        if ((i & 15) == 0) MARK("pull");
      } else if (++zero > 50) {
        break;
      } else {
        pump();
        Sleep(1);
      }
      if (got > 0 && got < (int)sizeof buf) break;  // short read = end
    }
    fclose(f);
    printf("cmd pcm total = %ld bytes (%.2f s @ %d Hz 16-bit)\n", total,
           total / 2.0 / (rate ? rate : 11025), rate);
    MARK("CloseBackend");
    printf("CloseBackend rc=%d\n", pCloseBE(scb));
  } else if (!strcmp(mode, "phon")) {
    MARK("TextToPhonetics");
    void *ph = pT2P(scb, text, 0);
    printf("phonetics=%p\n", ph);
    if (ph) {
      MARK("SpeakPhonetics");
      printf("SpeakPhonetics rc=%d\n", pSpeakP(scb, ph));
      for (int i = 0; i < 200; i++) {
        pump();
        MARK("poll");
        printf("  status=%d\n", pStatus(scb));
        Sleep(50);
      }
    }
  }

  MARK("CloseSpeech");
  printf("CloseSpeech rc=%d\n", pClose(scb));
  MARK("done");
  return 0;
}
