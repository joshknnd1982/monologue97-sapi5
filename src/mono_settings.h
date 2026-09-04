// mono_settings.h - the Custom Voice snapshot in
// HKCU\Software\Monologue97\CustomVoice.
//
// Values are stored as the 0..100 percentages the configuration utility shows,
// not as engine-native numbers. Storing native values would make the utility
// lie: percent -> native -> percent does not round-trip for the engine's small
// asymmetric ranges (Speed -5..14 turns a saved 50 % into 53 % on reopen), and
// a control that reads back a different number than the one just set is exactly
// the kind of thing that makes a screen reader confusing to use.

#pragma once

#include <windows.h>

#include <string>

#include "mono_voices.h"

namespace mono {

inline const wchar_t *kSettingsKey = L"Software\\Monologue97\\CustomVoice";

struct Settings {
  char font[16];          // speech font backing the Custom Voice
  int pct[kParamCount];   // 0..100 per parameter, in kParams order
};

inline Settings default_settings() {
  Settings s;
  strncpy_s(s.font, sizeof s.font, "ENMH", _TRUNCATE);
  for (int i = 0; i < kParamCount; i++)
    s.pct[i] = native_to_percent(i, kParams[i].def);
  return s;
}

inline Settings load_settings() {
  Settings s = default_settings();
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &k) !=
      ERROR_SUCCESS)
    return s;

  char buf[64];
  DWORD sz = sizeof buf, type = 0;
  if (RegQueryValueExA(k, "Font", nullptr, &type, (BYTE *)buf, &sz) ==
          ERROR_SUCCESS &&
      type == REG_SZ) {
    buf[sizeof buf - 1] = '\0';
    if (font_index(buf) >= 0) strncpy_s(s.font, sizeof s.font, buf, _TRUNCATE);
  }
  for (int i = 0; i < kParamCount; i++) {
    DWORD v = 0;
    sz = sizeof v;
    if (RegQueryValueExA(k, kParams[i].name, nullptr, &type, (BYTE *)&v, &sz) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
      int pct = (int)v;
      s.pct[i] = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }
  }
  RegCloseKey(k);
  return s;
}

inline bool save_settings(const Settings &s) {
  HKEY k = nullptr;
  DWORD disp = 0;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &k, &disp) != ERROR_SUCCESS)
    return false;
  RegSetValueExA(k, "Font", 0, REG_SZ, (const BYTE *)s.font,
                 (DWORD)strlen(s.font) + 1);
  for (int i = 0; i < kParamCount; i++) {
    DWORD v = (DWORD)s.pct[i];
    RegSetValueExA(k, kParams[i].name, 0, REG_DWORD, (const BYTE *)&v,
                   sizeof v);
  }
  RegCloseKey(k);
  return true;
}

// The engine-native parameters a Custom Voice utterance should use.
inline Params settings_to_params(const Settings &s) {
  Params p;
  for (int i = 0; i < kParamCount; i++)
    p.v[i] = percent_to_native(i, s.pct[i]);
  return p;
}

}  // namespace mono
