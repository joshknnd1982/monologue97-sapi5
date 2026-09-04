// mono_config.cpp - the Monologue '97 configuration utility.
//
// Edits the Custom Voice snapshot in HKCU\Software\Monologue97\CustomVoice: the
// speech font and every speech parameter, each presented as 0..100 % where 0 %
// is the engine's minimum and 100 % its maximum. The SAPI5 "Monologue 97 Custom
// Voice" token re-reads that snapshot for every utterance, so a saved change
// takes effect on the next thing spoken.
//
// Accessibility notes, because this is a screen-reader tool first:
//   * Numbers are edit boxes with spin buttons, never trackbars. MSAA reports a
//     trackbar's position as a percentage of its range, so a slider showing 5
//     gets announced as "55".
//   * Every control is preceded in the tab order by its own static label, which
//     is where MSAA takes an edit box's accessible name from.
//   * Every control is a tab stop and every label carries an access key.
//   * The status line is a real control, so changes to it can be read.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../src/mono_core.h"
#include "../src/mono_log.h"
#include "../src/mono_settings.h"
#include "../src/mono_voices.h"
#include "resource.h"

namespace {

HINSTANCE g_inst = nullptr;
HWND g_dlg = nullptr;
mono::Settings g_set;

// See the note in WM_INITDIALOG: this starts true on purpose.
bool g_loading = true;

mono::Engine *g_engine = nullptr;
std::wstring g_engine_dir;
std::string g_engine_err;

// --- audio playback for the Test button -----------------------------------
HWAVEOUT g_wave = nullptr;
WAVEHDR g_hdr;
std::vector<int16_t> g_pcm;

void stop_playback() {
  if (!g_wave) return;
  waveOutReset(g_wave);
  if (g_hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wave, &g_hdr, sizeof g_hdr);
  waveOutClose(g_wave);
  g_wave = nullptr;
  memset(&g_hdr, 0, sizeof g_hdr);
}

void set_status(const wchar_t *fmt, ...) {
  wchar_t buf[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  SetDlgItemTextW(g_dlg, IDC_STATUS, buf);
}

// --- parameter <-> control mapping -----------------------------------------
// kParams[0..4] are numeric (edit + spin), kParams[5..9] boolean (checkbox).
bool is_numeric(int i) { return !mono::kParams[i].boolean; }
int edit_id(int i) { return IDC_NUM_EDIT + i * 2; }
int spin_id(int i) { return IDC_NUM_EDIT + i * 2 + 1; }
int check_id(int i) { return IDC_CHK_FIRST + (i - 5); }

void controls_from_settings() {
  g_loading = true;

  int sel = 0;
  for (int i = 0, n = 0; i < mono::kFontCount; i++) {
    if (!g_engine || g_engine->font_available(i)) {
      if (_stricmp(mono::kFonts[i].name, g_set.font) == 0) sel = n;
      n++;
    }
  }
  SendDlgItemMessageW(g_dlg, IDC_VOICE, CB_SETCURSEL, sel, 0);

  for (int i = 0; i < mono::kParamCount; i++) {
    if (is_numeric(i)) {
      SetDlgItemInt(g_dlg, edit_id(i), g_set.pct[i], FALSE);
    } else {
      CheckDlgButton(g_dlg, check_id(i), g_set.pct[i] >= 50 ? BST_CHECKED
                                                            : BST_UNCHECKED);
    }
  }
  g_loading = false;
}

void settings_from_controls() {
  int sel = (int)SendDlgItemMessageW(g_dlg, IDC_VOICE, CB_GETCURSEL, 0, 0);
  if (sel >= 0) {
    int idx = (int)SendDlgItemMessageW(g_dlg, IDC_VOICE, CB_GETITEMDATA, sel, 0);
    if (idx >= 0 && idx < mono::kFontCount)
      strncpy_s(g_set.font, sizeof g_set.font, mono::kFonts[idx].name,
                _TRUNCATE);
  }
  for (int i = 0; i < mono::kParamCount; i++) {
    if (is_numeric(i)) {
      BOOL ok = FALSE;
      int v = (int)GetDlgItemInt(g_dlg, edit_id(i), &ok, FALSE);
      if (!ok) v = g_set.pct[i];
      g_set.pct[i] = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else {
      g_set.pct[i] =
          IsDlgButtonChecked(g_dlg, check_id(i)) == BST_CHECKED ? 100 : 0;
    }
  }
}

// --- test speech -----------------------------------------------------------
bool collect_pcm(const int16_t *s, size_t n, void *) {
  g_pcm.insert(g_pcm.end(), s, s + n);
  return true;
}

void do_test() {
  settings_from_controls();
  stop_playback();

  if (!g_engine) {
    set_status(L"Speech engine unavailable: %hs", g_engine_err.c_str());
    return;
  }

  wchar_t text[1024];
  GetDlgItemTextW(g_dlg, IDC_TESTTEXT, text, 1024);
  if (!text[0]) {
    set_status(L"Type some text to speak first.");
    SetFocus(GetDlgItem(g_dlg, IDC_TESTTEXT));
    return;
  }
  char ansi[2048];
  WideCharToMultiByte(CP_ACP, 0, text, -1, ansi, sizeof ansi, nullptr, nullptr);

  set_status(L"Speaking...");
  std::string err;
  if (!g_engine->select_font(g_set.font, &err)) {
    set_status(L"Cannot select that voice: %hs", err.c_str());
    return;
  }
  g_pcm.clear();
  const mono::Params p = mono::settings_to_params(g_set);
  if (!g_engine->render(ansi, p, collect_pcm, nullptr, &err)) {
    set_status(L"Speech failed: %hs", err.c_str());
    return;
  }
  if (g_pcm.empty()) {
    set_status(L"The engine produced no audio for that text.");
    return;
  }

  WAVEFORMATEX wfx;
  memset(&wfx, 0, sizeof wfx);
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = mono::kChannels;
  wfx.nSamplesPerSec = g_engine->sample_rate();
  wfx.wBitsPerSample = mono::kBitsPerSample;
  wfx.nBlockAlign = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
  if (waveOutOpen(&g_wave, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) !=
      MMSYSERR_NOERROR) {
    g_wave = nullptr;
    set_status(L"Could not open the sound device.");
    return;
  }
  memset(&g_hdr, 0, sizeof g_hdr);
  g_hdr.lpData = (LPSTR)g_pcm.data();
  g_hdr.dwBufferLength = (DWORD)(g_pcm.size() * sizeof(int16_t));
  waveOutPrepareHeader(g_wave, &g_hdr, sizeof g_hdr);
  waveOutWrite(g_wave, &g_hdr, sizeof g_hdr);
  set_status(L"Spoke %u characters (%.1f seconds).", (unsigned)wcslen(text),
             g_pcm.size() / (double)g_engine->sample_rate());
}

void open_log_folder() {
  wchar_t dir[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
    std::wstring p = std::wstring(dir) + L"\\Monologue97\\logs";
    CreateDirectoryW((std::wstring(dir) + L"\\Monologue97").c_str(), nullptr);
    CreateDirectoryW(p.c_str(), nullptr);
    ShellExecuteW(g_dlg, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    set_status(L"Opened the log folder.");
  }
}

INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      g_dlg = dlg;
      SendMessageW(dlg, WM_SETICON, ICON_BIG,
                   (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON)));
      SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                   (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON)));

      // Populate the voice list. Item data is the index into kFonts, so the
      // list can hide voices that are not installed without the selection
      // logic having to care.
      for (int i = 0; i < mono::kFontCount; i++) {
        if (g_engine && !g_engine->font_available(i)) continue;
        wchar_t label[128];
        MultiByteToWideChar(CP_ACP, 0, mono::kFonts[i].sapi, -1, label, 128);
        int n = (int)SendDlgItemMessageW(dlg, IDC_VOICE, CB_ADDSTRING, 0,
                                         (LPARAM)label);
        SendDlgItemMessageW(dlg, IDC_VOICE, CB_SETITEMDATA, n, i);
      }

      for (int i = 0; i < mono::kParamCount; i++) {
        if (!is_numeric(i)) continue;
        HWND spin = GetDlgItem(dlg, spin_id(i));
        SendMessageW(spin, UDM_SETBUDDY, (WPARAM)GetDlgItem(dlg, edit_id(i)), 0);
        SendMessageW(spin, UDM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(dlg, edit_id(i), EM_SETLIMITTEXT, 3, 0);
        // The spin buttons are not tab stops -- the value is changed with the
        // arrow keys from inside the edit box -- but give them a name anyway
        // so a review cursor or a mouse pointer landing on one says what it
        // belongs to instead of nothing.
        wchar_t label[64];
        _snwprintf_s(label, _TRUNCATE, L"%hs up-down", mono::kParams[i].label);
        SetWindowTextW(spin, label);
      }

      SetDlgItemTextW(dlg, IDC_TESTTEXT,
                      L"Monologue ninety seven is ready. The quick brown fox "
                      L"jumps over the lazy dog.");

      controls_from_settings();

      if (!g_engine)
        set_status(L"Settings can be saved, but the speech engine could not be "
                   L"loaded: %hs",
                   g_engine_err.c_str());
      else
        set_status(L"Ready. 0 percent is the slowest or lowest setting, 100 "
                   L"percent the fastest or highest.");
      return TRUE;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wp);
      const int code = HIWORD(wp);

      // A spin control's buddy edit fires EN_CHANGE while the dialog is still
      // being created -- before WM_INITDIALOG runs -- so g_loading starts true
      // and is only cleared once the real values are in place. Without that,
      // those pre-init notifications would read empty edit boxes and overwrite
      // the user's saved settings with zeroes.
      if (g_loading) return FALSE;

      switch (id) {
        case IDC_TEST:
          do_test();
          return TRUE;
        case IDC_STOP:
          stop_playback();
          set_status(L"Stopped.");
          return TRUE;
        case IDC_LOGS:
          open_log_folder();
          return TRUE;
        case IDC_DEFAULTS:
          g_set = mono::default_settings();
          controls_from_settings();
          set_status(L"Restored the default settings. Choose Save and close to "
                     L"keep them.");
          return TRUE;
        case IDOK:
          settings_from_controls();
          if (mono::save_settings(g_set)) {
            MONO_LOG("settings saved: font=%s", g_set.font);
            stop_playback();
            EndDialog(dlg, IDOK);
          } else {
            set_status(L"Could not save the settings.");
          }
          return TRUE;
        case IDCANCEL:
          stop_playback();
          EndDialog(dlg, IDCANCEL);
          return TRUE;
        default:
          break;
      }

      if (id == IDC_VOICE && code == CBN_SELCHANGE) {
        settings_from_controls();
        set_status(L"Voice set. Choose Speak test to hear it.");
        return TRUE;
      }
      return FALSE;
    }

    case WM_CLOSE:
      stop_playback();
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }
  return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  g_inst = inst;
  mono::log_init("config");

  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof icc;
  icc.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
  InitCommonControlsEx(&icc);

  // The engine lives in an "engine" folder next to this executable when
  // installed, or in the sibling folder when run from a build tree.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  size_t slash = dir.find_last_of(L'\\');
  if (slash != std::wstring::npos) dir.resize(slash);
  g_engine_dir = dir + L"\\engine";
  if (GetFileAttributesW((g_engine_dir + L"\\mnvox11.dll").c_str()) ==
      INVALID_FILE_ATTRIBUTES) {
    size_t up = dir.find_last_of(L'\\');
    if (up != std::wstring::npos)
      g_engine_dir = dir.substr(0, up) + L"\\engine";
  }

  // The utility stays usable for editing and saving even if the engine will not
  // load; only the Test button needs it.
  g_engine = new mono::Engine();
  if (!g_engine->init(g_engine_dir, &g_engine_err)) {
    MONO_LOG("engine unavailable: %s", g_engine_err.c_str());
    delete g_engine;
    g_engine = nullptr;
  }

  g_set = mono::load_settings();
  INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CONFIG), nullptr,
                              dlg_proc, 0);
  stop_playback();
  delete g_engine;
  return r == IDOK ? 0 : 1;
}
