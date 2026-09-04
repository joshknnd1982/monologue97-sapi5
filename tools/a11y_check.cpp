// a11y_check.cpp - verify the configuration utility is usable by a screen
// reader: every control reachable by Tab, every control with an accessible
// name, and the name being the label the user expects.
//
// Uses MSAA (oleacc) deliberately. A UI Automation client -- including
// PowerShell's -- reports plain Win32 dialog controls as generic "Pane"
// elements with no useful name, which makes it look like everything is broken
// when it is fine. oleacc reports what NVDA and Narrator actually consume.
//
//   a11y_check <path-to-Monologue97Config.exe>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <oleacc.h>
#include <stdio.h>

#include <string>
#include <vector>

static const wchar_t *role_name(long role) {
  switch (role) {
    case ROLE_SYSTEM_PUSHBUTTON: return L"button";
    case ROLE_SYSTEM_CHECKBUTTON: return L"check box";
    case ROLE_SYSTEM_COMBOBOX: return L"combo box";
    case ROLE_SYSTEM_TEXT: return L"edit";
    case ROLE_SYSTEM_STATICTEXT: return L"static";
    case ROLE_SYSTEM_SPINBUTTON: return L"spinner";
    case ROLE_SYSTEM_GROUPING: return L"group";
    case ROLE_SYSTEM_DIALOG: return L"dialog";
    case ROLE_SYSTEM_WINDOW: return L"window";
    case ROLE_SYSTEM_CLIENT: return L"client";
    default: return L"other";
  }
}

struct Ctl {
  HWND hwnd;
  std::wstring cls, name;
  long role;
  bool tabstop, visible;
};

static std::wstring acc_name(HWND h) {
  IAccessible *acc = nullptr;
  std::wstring out;
  if (SUCCEEDED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT,
                                           IID_IAccessible, (void **)&acc)) &&
      acc) {
    VARIANT self;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    BSTR b = nullptr;
    if (SUCCEEDED(acc->get_accName(self, &b)) && b) {
      out = b;
      SysFreeString(b);
    }
    acc->Release();
  }
  return out;
}

static long acc_role(HWND h) {
  IAccessible *acc = nullptr;
  long r = 0;
  if (SUCCEEDED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT,
                                           IID_IAccessible, (void **)&acc)) &&
      acc) {
    VARIANT self, role;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VariantInit(&role);
    if (SUCCEEDED(acc->get_accRole(self, &role)) && role.vt == VT_I4)
      r = role.lVal;
    VariantClear(&role);
    acc->Release();
  }
  return r;
}

static std::vector<Ctl> g_ctls;

static BOOL CALLBACK enum_child(HWND h, LPARAM) {
  wchar_t cls[64] = L"";
  GetClassNameW(h, cls, 64);
  Ctl c;
  c.hwnd = h;
  c.cls = cls;
  c.name = acc_name(h);
  c.role = acc_role(h);
  LONG style = GetWindowLongW(h, GWL_STYLE);
  c.tabstop = (style & WS_TABSTOP) != 0;
  c.visible = (style & WS_VISIBLE) != 0;
  g_ctls.push_back(c);
  return TRUE;
}

static HWND g_dlg = nullptr;
static DWORD g_pid = 0;

static BOOL CALLBACK find_dlg(HWND h, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != g_pid || !IsWindowVisible(h)) return TRUE;
  wchar_t title[256] = L"";
  GetWindowTextW(h, title, 256);
  if (wcsstr(title, L"Monologue")) {
    g_dlg = h;
    return FALSE;
  }
  return TRUE;
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    printf("usage: a11y_check <Monologue97Config.exe>\n");
    return 1;
  }
  CoInitialize(nullptr);

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  std::wstring cmd = std::wstring(L"\"") + argv[1] + L"\"";
  std::vector<wchar_t> mut(cmd.begin(), cmd.end());
  mut.push_back(0);
  if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &si, &pi)) {
    printf("cannot start the utility: %lu\n", GetLastError());
    return 2;
  }
  g_pid = pi.dwProcessId;
  WaitForInputIdle(pi.hProcess, 10000);

  for (int i = 0; i < 100 && !g_dlg; i++) {
    EnumWindows(find_dlg, 0);
    if (!g_dlg) Sleep(100);
  }
  if (!g_dlg) {
    printf("FAIL: the configuration dialog never appeared\n");
    TerminateProcess(pi.hProcess, 1);
    return 3;
  }
  wchar_t title[256];
  GetWindowTextW(g_dlg, title, 256);
  printf("dialog: \"%ls\"  (accessible name: \"%ls\", role %ls)\n\n", title,
         acc_name(g_dlg).c_str(), role_name(acc_role(g_dlg)));

  EnumChildWindows(g_dlg, enum_child, 0);

  printf("--- controls ---\n");
  printf("%-18s %-11s %-4s %s\n", "class", "role", "tab", "accessible name");
  int unnamed = 0;
  for (size_t i = 0; i < g_ctls.size(); i++) {
    const Ctl &c = g_ctls[i];
    if (!c.visible) continue;
    // Labels and group boxes are names for other controls, not things a user
    // lands on. A spin button that is not a tab stop is likewise never focused
    // -- its value is edited from its buddy edit box with the arrow keys -- so
    // it is reported but not required to carry a name of its own.
    const bool needs_name = c.role != ROLE_SYSTEM_STATICTEXT &&
                            c.role != ROLE_SYSTEM_GROUPING &&
                            (c.tabstop || c.role != ROLE_SYSTEM_SPINBUTTON);
    if (needs_name && c.name.empty()) unnamed++;
    printf("%-18ls %-11ls %-4s %ls%s\n", c.cls.c_str(), role_name(c.role),
           c.tabstop ? "yes" : "-", c.name.c_str(),
           (needs_name && c.name.empty()) ? "   <-- NO NAME" : "");
  }

  // Walk the dialog's tab ring the way pressing Tab does, and check it visits
  // every focusable control and comes back around.
  printf("\n--- tab order ---\n");
  HWND first = GetNextDlgTabItem(g_dlg, nullptr, FALSE);
  HWND h = first;
  int steps = 0, named = 0;
  std::vector<HWND> visited;
  while (h && steps < 64) {
    std::wstring n = acc_name(h);
    printf("  %2d. %-11ls %ls%s\n", steps + 1, role_name(acc_role(h)),
           n.c_str(), n.empty() ? "   <-- NO NAME" : "");
    if (!n.empty()) named++;
    visited.push_back(h);
    steps++;
    h = GetNextDlgTabItem(g_dlg, h, FALSE);
    if (h == first) break;
  }
  printf("  %d controls in the tab ring, %d with an accessible name\n", steps,
         named);

  int tabstops = 0;
  for (size_t i = 0; i < g_ctls.size(); i++)
    if (g_ctls[i].visible && g_ctls[i].tabstop) tabstops++;

  printf("\n--- result ---\n");
  bool ok = true;
  if (unnamed) {
    printf("FAIL: %d interactive control(s) have no accessible name\n", unnamed);
    ok = false;
  }
  if (steps < tabstops) {
    printf("FAIL: %d controls carry WS_TABSTOP but only %d are in the tab "
           "ring\n",
           tabstops, steps);
    ok = false;
  }
  if (named < steps) {
    printf("FAIL: %d control(s) in the tab ring have no name\n", steps - named);
    ok = false;
  }
  if (ok)
    printf("PASS: every control is reachable by Tab and announces a name\n");

  PostMessageW(g_dlg, WM_CLOSE, 0, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  TerminateProcess(pi.hProcess, 0);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CoUninitialize();
  return ok ? 0 : 4;
}
