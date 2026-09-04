// probe.cpp - Reverse-engineering probe for the First Byte / PrimoVOX
// "Monologue '97" speech engine (mnvox11.dll, 32-bit).
//
// Findings this probe verifies:
//   * every entry point is __stdcall
//   * OpenSpeech enumerates speech fonts from
//         HKLM\Software\FirstByte\PrimoVOX\SpeechFonts
//     reading the "Path" value for the font directory and treating each
//     subkey name NAME as <Path>\NAME.DLL (validated via its VERSIONNUMBER
//     RCDATA resource: major >= 1, minor < 10).
//   * that requirement is satisfied without touching real HKLM, and without
//     administrator rights, by pointing HKEY_LOCAL_MACHINE at a private
//     volatile key with RegOverridePredefKey for the duration of the call.
//   * the speech-parameter ID space and each parameter's clamping range.
//   * audio capture: OpenBackend -> WAV file, GetPCMdata -> memory.
//
// Build: x86 only (the engine is a 32-bit PE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

typedef void *LPSCB;

typedef WORD(__stdcall *PFN_SpeechVersion)(void);
typedef int(__stdcall *PFN_PrimoVOXPresent)(void);
typedef LPSCB(__stdcall *PFN_OpenSpeech)(void *a, void *b, const char *font);
typedef int(__stdcall *PFN_CloseSpeech)(LPSCB);
typedef int(__stdcall *PFN_Say)(LPSCB, const char *);
typedef int(__stdcall *PFN_SetSpeechParameter)(LPSCB, int param, int value);
typedef int(__stdcall *PFN_GetSpeechParameter)(LPSCB, int param, int *value);
typedef int(__stdcall *PFN_SpeechStatus)(LPSCB);
typedef int(__stdcall *PFN_OpenBackend)(LPSCB, const char *filename);
typedef int(__stdcall *PFN_CloseBackend)(LPSCB);
typedef int(__stdcall *PFN_GetPCMdata)(LPSCB, void *buf, int nbytes);
typedef int(__stdcall *PFN_ResetSpeech)(LPSCB);
typedef int(__stdcall *PFN_ChangeSpeech)(LPSCB, void *, const char *);

static HMODULE g_dll;
#define GP(name) ((PFN_##name)GetProcAddress(g_dll, #name))

static const char *kParamName[] = {
    "Volume", "Pitch", "Speed", "?3", "?4", "?5", "?6", "?7", "?8", "?9",
    "?10", "?11", "?12", "?13", "?14", "?15", "?16", "?17", "?18", "?19",
    "?20", "?21", "?22", "Brightness", "Emphasis", "Nasal", "Oral", "Breathy",
    "Whispery", "Creaky", "?30", "?31", "?32", "?33", "?34"};

// The 21 fonts shipped on the Monologue '97 CD, with the display names from
// CDSF.INI. (CDSF.INI misspells HELIUM.DLL as HELLIUM.DLL; the file on the CD
// is HELIUM.DLL.) Each font's registry subkey needs a "Description" value --
// mnvox11 reads it with RegQueryValueEx into a buffer it never initialises, so
// a missing value makes OpenSpeech fault.
struct Font {
  const char *name;
  const char *desc;
};
static const Font kFonts[] = {
    {"ENMH", "U.S. Male"},          {"ENFH", "U.S. Female"},
    {"BREATHY", "MaleBreathy"},     {"DEEP", "MaleDeep"},
    {"DEEPER", "MaleDeeper"},       {"HELIUM", "MaleHelium"},
    {"NASAL", "MaleImpediment"},    {"ORAL", "MaleHeadcold"},
    {"SOFT", "MaleSoft"},           {"SPACE", "MaleChime"},
    {"VERYHIGH", "MaleVeryHigh"},   {"WHISPER", "MaleWhisper"},
    {"FBREATHY", "FemBreathy"},     {"FDEEP", "FemDeep"},
    {"FDEEPER", "FemGravelly"},     {"FHIGHER", "FemHigher"},
    {"FORAL", "FemHeadcold"},       {"FSMOKEY", "FemSmokey"},
    {"FWHISPER", "FemWhisper"},     {"MONOTONE", "Monotone"},
    {"XYLON", "XylonRobot"}};

static const char *kRegPath =
    "Software\\FirstByte\\PrimoVOX\\SpeechFonts";

static HKEY g_view;  // private HKLM stand-in

// Report the faulting address relative to mnvox11.dll so a crash can be
// mapped straight back to an RVA in the disassembly.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
  void *addr = ep->ExceptionRecord->ExceptionAddress;
  printf("\n*** EXCEPTION %08lX at %p",
         ep->ExceptionRecord->ExceptionCode, addr);
  if (g_dll && (ULONG_PTR)addr >= (ULONG_PTR)g_dll)
    printf("  (mnvox11.dll+0x%lX)",
           (unsigned long)((ULONG_PTR)addr - (ULONG_PTR)g_dll));
  if (ep->ExceptionRecord->NumberParameters >= 2)
    printf("  op=%lu addr=%p",
           (unsigned long)ep->ExceptionRecord->ExceptionInformation[0],
           (void *)ep->ExceptionRecord->ExceptionInformation[1]);
  printf("\n");
  fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// The engine reports problems with modal MessageBoxes ("Could not load Speech
// Font.", "OpenSpeech failed.", ...). In a service, a SAPI host or a screen
// reader those would hang the caller forever, so redirect mnvox11's import of
// USER32!MessageBoxA to a stub that logs the text and answers IDOK.
static int __stdcall fake_messagebox(HWND hwnd, LPCSTR text, LPCSTR cap,
                                     UINT type) {
  (void)hwnd;
  (void)type;
  printf("\n[engine MessageBox suppressed] %s: %s\n", cap ? cap : "(null)",
         text ? text : "(null)");
  fflush(stdout);
  return IDOK;
}

static BOOL patch_import(HMODULE mod, const char *dll, const char *fn,
                         void *repl, void **orig) {
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  DWORD rva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  if (!rva) return FALSE;
  IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
  for (; imp->Name; imp++) {
    const char *name = (const char *)(base + imp->Name);
    if (_stricmp(name, dll) != 0) continue;
    IMAGE_THUNK_DATA *oft =
        (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk
                                         ? imp->OriginalFirstThunk
                                         : imp->FirstThunk));
    IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; oft->u1.AddressOfData; oft++, ft++) {
      if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
      IMAGE_IMPORT_BY_NAME *ibn =
          (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
      if (strcmp((const char *)ibn->Name, fn) != 0) continue;
      DWORD old;
      if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE,
                          &old))
        return FALSE;
      if (orig) *orig = (void *)(ULONG_PTR)ft->u1.Function;
      ft->u1.Function = (ULONG_PTR)repl;
      VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
      return TRUE;
    }
  }
  return FALSE;
}

// A vectored handler runs before any SEH frame and on every thread, so it sees
// faults raised on the engine's own worker threads too.
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION ||
      code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_PRIV_INSTRUCTION ||
      code == EXCEPTION_STACK_OVERFLOW ||
      code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    printf("\n[VEH] code=%08lX at %p", code, addr);
    if (g_dll && (ULONG_PTR)addr >= (ULONG_PTR)g_dll &&
        (ULONG_PTR)addr < (ULONG_PTR)g_dll + 0x60000)
      printf(" = mnvox11+0x%lX",
             (unsigned long)((ULONG_PTR)addr - (ULONG_PTR)g_dll));
    if (ep->ExceptionRecord->NumberParameters >= 2)
      printf("  %s %p",
             ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
             (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    printf("  tid=%lu\n", GetCurrentThreadId());
    fflush(stdout);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Sampling watchdog: after a delay, suspend every thread in the process in turn
// and report its instruction pointer, resolved against the loaded modules. That
// localises a hang inside the engine without a debugger.
static const char *module_of(ULONG_PTR ip, ULONG_PTR *off) {
  static char buf[64];
  HMODULE mods[128];
  DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
    return NULL;
  const char *best = NULL;
  ULONG_PTR bestbase = 0;
  for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi))
      continue;
    ULONG_PTR b = (ULONG_PTR)mi.lpBaseOfDll;
    if (ip >= b && ip < b + mi.SizeOfImage && b > bestbase) {
      GetModuleBaseNameA(GetCurrentProcess(), mods[i], buf, sizeof buf);
      best = buf;
      bestbase = b;
    }
  }
  if (best) *off = ip - bestbase;
  return best;
}

static DWORD WINAPI watchdog(LPVOID param) {
  DWORD delay = (DWORD)(ULONG_PTR)param;
  Sleep(delay);
  for (int round = 0; round < 3; round++) {
    printf("\n[watchdog] thread sample %d\n", round);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        if (te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                               te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(th, &c)) {
          ULONG_PTR off = 0;
          const char *m = module_of((ULONG_PTR)c.Eip, &off);
          printf("   tid %5lu  eip=%08lX", te.th32ThreadID,
                 (unsigned long)c.Eip);
          if (m)
            printf("  %s+0x%lX", m, (unsigned long)off);
          printf("\n");
          // No symbols available, so scan the raw stack for return addresses
          // that land inside mnvox11 - enough to see which engine call blocked.
          ULONG_PTR *sp = (ULONG_PTR *)c.Esp;
          int shown = 0;
          for (int k = 0; k < 4096 && shown < 12; k++) {
            ULONG_PTR v = 0;
            if (IsBadReadPtr(sp + k, sizeof(ULONG_PTR))) break;
            v = sp[k];
            if (g_dll && v > (ULONG_PTR)g_dll &&
                v < (ULONG_PTR)g_dll + 0x60000) {
              printf("        stack[%4d] -> mnvox11+0x%lX\n", k,
                     (unsigned long)(v - (ULONG_PTR)g_dll));
              shown++;
            }
          }
        }
        ResumeThread(th);
        CloseHandle(th);
      } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    fflush(stdout);
    Sleep(1500);
  }
  printf("[watchdog] giving up, exiting process\n");
  fflush(stdout);
  ExitProcess(99);
  return 0;
}

static void pump(void) {
  MSG m;
  while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

// Build HKCU\<root>\Software\FirstByte\PrimoVOX\SpeechFonts with Path plus one
// subkey per font, then alias HKEY_LOCAL_MACHINE onto it.
static BOOL build_view(const char *sfdir, const char *only) {
  // RegDeleteKey refuses a key that still has subkeys, so every earlier run's
  // fonts would otherwise survive and contaminate the experiment.
  RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\MonologueProbe");
  HKEY root;
  DWORD disp;
  LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MonologueProbe\\HKLM",
                            0, NULL, REG_OPTION_VOLATILE,
                            KEY_ALL_ACCESS, NULL, &root, &disp);
  if (rc != ERROR_SUCCESS) {
    printf("RegCreateKeyEx(root) failed %ld\n", rc);
    return FALSE;
  }
  HKEY fonts;
  rc = RegCreateKeyExA(root, kRegPath, 0, NULL, REG_OPTION_VOLATILE,
                       KEY_ALL_ACCESS, NULL, &fonts, &disp);
  if (rc != ERROR_SUCCESS) {
    printf("RegCreateKeyEx(SpeechFonts) failed %ld\n", rc);
    return FALSE;
  }
  RegSetValueExA(fonts, "Path", 0, REG_SZ, (const BYTE *)sfdir,
                 (DWORD)strlen(sfdir) + 1);
  for (int i = 0; i < (int)(sizeof kFonts / sizeof kFonts[0]); i++) {
    if (only) {  // only == ",ENMH,ENFH," style filter
      char pat[64];
      _snprintf(pat, sizeof pat, ",%s,", kFonts[i].name);
      if (!strstr(only, pat)) continue;
    }
    char probe[MAX_PATH];
    _snprintf(probe, sizeof probe, "%s\\%s.DLL", sfdir, kFonts[i].name);
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
      printf("  (skip %s - no %s)\n", kFonts[i].name, probe);
      continue;
    }
    printf("  register %s = %s\n", kFonts[i].name, kFonts[i].desc);
    HKEY k;
    if (RegCreateKeyExA(fonts, kFonts[i].name, 0, NULL, REG_OPTION_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &k, &disp) == ERROR_SUCCESS) {
      RegSetValueExA(k, "Description", 0, REG_SZ,
                     (const BYTE *)kFonts[i].desc,
                     (DWORD)strlen(kFonts[i].desc) + 1);
      RegCloseKey(k);
    }
  }
  RegCloseKey(fonts);
  g_view = root;
  LONG orc = RegOverridePredefKey(HKEY_LOCAL_MACHINE, root);
  printf("RegOverridePredefKey(HKLM) rc=%ld\n", orc);
  return orc == ERROR_SUCCESS;
}

static void drop_view(void) {
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
}

int main(int argc, char **argv) {
  const char *engdir = (argc > 1) ? argv[1] : ".";
  const char *font = (argc > 2) ? argv[2] : "ENMH";
  char sfdir[MAX_PATH];
  _snprintf(sfdir, sizeof sfdir, "%s\\SF", engdir);

  SetCurrentDirectoryA(engdir);
  SetDllDirectoryA(engdir);
  char dllpath[MAX_PATH];
  _snprintf(dllpath, sizeof dllpath, "%s\\mnvox11.dll", engdir);
  g_dll = LoadLibraryA(dllpath);
  if (!g_dll) {
    printf("LoadLibrary(%s) failed: %lu\n", dllpath, GetLastError());
    return 1;
  }
  printf("mnvox11.dll loaded at %p\n", (void *)g_dll);
  printf("MessageBoxA hook: %s\n",
         patch_import(g_dll, "USER32.dll", "MessageBoxA",
                      (void *)fake_messagebox, NULL)
             ? "installed"
             : "NOT installed");

  PFN_SpeechVersion pVersion = GP(SpeechVersion);
  PFN_OpenSpeech pOpen = GP(OpenSpeech);
  PFN_CloseSpeech pClose = GP(CloseSpeech);
  PFN_Say pSay = GP(Say);
  PFN_SetSpeechParameter pSet = GP(SetSpeechParameter);
  PFN_GetSpeechParameter pGet = GP(GetSpeechParameter);
  PFN_SpeechStatus pStatus = GP(SpeechStatus);
  PFN_OpenBackend pOpenBE = GP(OpenBackend);
  PFN_CloseBackend pCloseBE = GP(CloseBackend);
  PFN_GetPCMdata pPCM = GP(GetPCMdata);

  printf("SpeechVersion = 0x%04X\n", pVersion());
  printf("SF dir = %s\n", sfdir);

  // argv[3]: comma-separated font list to register (default: all).
  char only[1024] = "";
  const char *onlyp = NULL;
  if (argc > 3) {
    _snprintf(only, sizeof only, ",%s,", argv[3]);
    onlyp = only;
  }
  if (!build_view(sfdir, onlyp)) return 2;

  SetUnhandledExceptionFilter(crash_filter);
  CreateThread(NULL, 0, watchdog, (LPVOID)(ULONG_PTR)60000, 0, NULL);
  AddVectoredExceptionHandler(1, veh);

  // OpenSpeech's first argument is an HWND, not an HINSTANCE: it is stored in
  // four (hwnd, message) notification slots (messages 100..103), and Say spins
  // on PeekMessage(hwnd, ...) waiting for the completion message. Passing
  // anything that is not a real window owned by *this* thread makes Say spin
  // forever, so create a message-only window first.
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "MonologueProbeWnd";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "MonologueProbeWnd", "monologue", 0, 0, 0, 0,
                              0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
  printf("notify hwnd = %p\n", (void *)hwnd);

  LPSCB scb = NULL;
  __try {
    printf("OpenSpeech(hwnd, NULL, \"%s\") ... ", font);
    fflush(stdout);
    scb = pOpen(hwnd, NULL, font);
    printf("-> %p\n", scb);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    printf("\nOpenSpeech raised 0x%08lX\n", GetExceptionCode());
    fflush(stdout);
    scb = NULL;
  }
  drop_view();
  if (!scb) {
    printf("OpenSpeech failed.\n");
    return 3;
  }

  // Font table built by OpenSpeech: count at scb+0x14C0, array at scb+0x14BC,
  // stride 0x8A8.
  {
    unsigned char *s = (unsigned char *)scb;
    DWORD count = *(DWORD *)(s + 0x14C0);
    unsigned char *arr = *(unsigned char **)(s + 0x14BC);
    printf("\n--- speech fonts loaded: %lu ---\n", count);
    for (DWORD i = 0; i < count && i < 40 && arr; i++) {
      unsigned char *e = arr + i * 0x8A8;
      printf("  [%2lu] name=%.32s\n", i, (char *)e);
    }
  }

  printf("\n--- GetSpeechParameter defaults ---\n");
  for (int p = 0; p <= 34; p++) {
    int v = -12345;
    int rc = pGet(scb, p, &v);
    printf("  %2d %-12s rc=%-3d value=%d\n", p, kParamName[p], rc, v);
  }

  // Sweep only the documented user parameters. IDs 3..22 and 30..34 are
  // internal slots (notification HWND/message pairs, buffer pointers, the
  // sample rate at 20 and the format flag at 21); writing to them corrupts the
  // control block and faults the engine.
  static const int kUserParams[] = {0, 1, 2, 23, 24, 25, 26, 27, 28, 29};
  printf("\n--- Accepted value ranges (SetSpeechParameter rc==0) ---\n");
  for (int ip = 0; ip < (int)(sizeof kUserParams / sizeof kUserParams[0]);
       ip++) {
    int p = kUserParams[ip];
    int lo = 99999, hi = -99999, nacc = 0;
    for (int v = -300; v <= 400; v++) {
      if (pSet(scb, p, v) == 0) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        nacc++;
      }
    }
    if (!nacc)
      printf("  %2d %-12s (none accepted)\n", p, kParamName[p]);
    else
      printf("  %2d %-12s %d .. %d%s\n", p, kParamName[p], lo, hi,
             nacc == (hi - lo + 1) ? "" : " (sparse)");
  }

  pSet(scb, 0, 9);
  pSet(scb, 1, 5);
  pSet(scb, 2, 5);

  const char *text =
      "Monologue ninety seven, the First Byte Primo VOX speech engine.";

  printf("\n--- OpenBackend WAV render ---\n");
  char wav[MAX_PATH];
  _snprintf(wav, sizeof wav, "%s\\probe_out.wav", engdir);
  DeleteFileA(wav);
  int rc = pOpenBE(scb, wav);
  printf("OpenBackend rc=%d\n", rc);
  if (rc == 0) {
    printf("Say rc=%d\n", pSay(scb, text));
    for (int i = 0; i < 4000; i++) {
      pump();
      int st = pStatus(scb);
      if (i % 400 == 0) printf("  status=%d\n", st);
      if (st == 0 && i > 10) break;
      Sleep(5);
    }
    printf("CloseBackend rc=%d\n", pCloseBE(scb));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(wav, GetFileExInfoStandard, &fad))
      printf("WAV written: %lu bytes\n", fad.nFileSizeLow);
    else
      printf("WAV NOT created\n");
  }

  printf("\n--- GetPCMdata pull render ---\n");
  printf("Say rc=%d\n", pSay(scb, text));
  static unsigned char buf[4096];
  long total = 0;
  int empty = 0;
  FILE *raw = fopen("probe_pcm.raw", "wb");
  for (int i = 0; i < 6000; i++) {
    pump();
    int got = pPCM(scb, buf, sizeof buf);
    if (got > 0) {
      if (raw) fwrite(buf, 1, got, raw);
      total += got;
      empty = 0;
    } else if (++empty > 200) {
      break;
    } else {
      Sleep(2);
    }
  }
  if (raw) fclose(raw);
  printf("GetPCMdata total = %ld bytes\n", total);

  printf("\nCloseSpeech rc=%d\n", pClose(scb));
  return 0;
}
