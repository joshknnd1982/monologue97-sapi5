// mono_voices.h - the Monologue '97 voice table and speech-parameter model.
//
// Shared by the engine core, the 32-bit host, both SAPI5 dlls and the
// configuration utility, so there is exactly one description of what a voice is
// and what each parameter's range means.

#pragma once

#include <stdint.h>

namespace mono {

// --- Speech fonts ---------------------------------------------------------
//
// A "speech font" is a resource-only dll in the engine's SF directory.
// ENMH/ENFH are the two base fonts and carry the whole synthesis data set; the
// other 19 are small modifier fonts with a BASELINE resource plus prosody
// overrides. Display names are the ones the original CDSF.INI used.
//
// `female` drives the SAPI Gender attribute. `base` marks the two full fonts.
struct FontInfo {
  const char *name;     // registry subkey and <SF>\NAME.DLL
  const char *display;  // CDSF.INI description
  const char *sapi;     // SAPI5 voice token display name
  bool female;
  bool base;
};

inline const FontInfo kFonts[] = {
    {"ENMH", "U.S. Male", "Monologue US Male", false, true},
    {"ENFH", "U.S. Female", "Monologue US Female", true, true},
    {"BREATHY", "MaleBreathy", "Monologue Male Breathy", false, false},
    {"DEEP", "MaleDeep", "Monologue Male Deep", false, false},
    {"DEEPER", "MaleDeeper", "Monologue Male Deeper", false, false},
    {"HELIUM", "MaleHelium", "Monologue Male Helium", false, false},
    {"NASAL", "MaleImpediment", "Monologue Male Impediment", false, false},
    {"ORAL", "MaleHeadcold", "Monologue Male Head Cold", false, false},
    {"SOFT", "MaleSoft", "Monologue Male Soft", false, false},
    {"SPACE", "MaleChime", "Monologue Male Chime", false, false},
    {"VERYHIGH", "MaleVeryHigh", "Monologue Male Very High", false, false},
    {"WHISPER", "MaleWhisper", "Monologue Male Whisper", false, false},
    {"FBREATHY", "FemBreathy", "Monologue Female Breathy", true, false},
    {"FDEEP", "FemDeep", "Monologue Female Deep", true, false},
    {"FDEEPER", "FemGravelly", "Monologue Female Gravelly", true, false},
    {"FHIGHER", "FemHigher", "Monologue Female Higher", true, false},
    {"FORAL", "FemHeadcold", "Monologue Female Head Cold", true, false},
    {"FSMOKEY", "FemSmokey", "Monologue Female Smokey", true, false},
    {"FWHISPER", "FemWhisper", "Monologue Female Whisper", true, false},
    {"MONOTONE", "Monotone", "Monologue Monotone", false, false},
    {"XYLON", "XylonRobot", "Monologue Xylon Robot", false, false},
};
inline const int kFontCount = (int)(sizeof(kFonts) / sizeof(kFonts[0]));

inline int font_index(const char *name) {
  for (int i = 0; i < kFontCount; i++) {
    const char *a = kFonts[i].name, *b = name;
    while (*a && *b &&
           ((*a | 32) == (*b | 32)))
      a++, b++;
    if (!*a && !*b) return i;
  }
  return -1;
}

// --- Speech parameters ----------------------------------------------------
//
// IDs and ranges established by disassembling SetSpeechParameter and confirmed
// by sweeping every value against the live engine. Only these ten IDs are real
// parameters; 3..22 and 30..34 are internal control-block slots (notification
// window/message pairs, buffer pointers, the sample rate at 20 and the format
// flag at 21) and writing to them corrupts the engine.
enum ParamId {
  PARAM_VOLUME = 0,      // 0..9   (-1 selects the font's own default)
  PARAM_PITCH = 1,       // -5..15
  PARAM_SPEED = 2,       // -5..15
  PARAM_BRIGHTNESS = 23, // -30..30
  PARAM_EMPHASIS = 24,   // 1..99
  PARAM_NASAL = 25,      // 0/1
  PARAM_ORAL = 26,       // 0/1
  PARAM_BREATHY = 27,    // 0/1
  PARAM_WHISPERY = 28,   // 0/1
  PARAM_CREAKY = 29,     // 0/1
  PARAM_SAMPLE_RATE = 20 // read-only: 11025
};

struct ParamSpec {
  ParamId id;
  const char *name;   // engine/registry name
  const char *label;  // configuration-utility label
  int lo, hi, def;
  bool boolean;
};

// Order here is the order the configuration utility presents them in.
//
// Two ranges are deliberately narrower than what SetSpeechParameter accepts:
//
//   Speed  the engine takes -5..15 but treats Speed as a linear duration
//          scale that reaches exactly zero at 15, so 15 is accepted and then
//          renders *nothing at all*. Publishing 15 as "100 %" would let a user
//          silence their own screen reader, so the usable maximum is 14.
//   Pitch  SetSpeechParameter accepts -5..15, but measuring F0 across the whole
//          range shows it only responds between 0 and 10: every value at or
//          below 0 renders byte-identically (76 Hz), and 10 and above all sit
//          at 200 Hz. Publishing the accepted range would leave half of a
//          host's pitch slider doing nothing at all.
//   Volume 0..9 is honoured in full, but the engine's output is already hot:
//          at 9 about 14 % of samples clip, while 5 is clean. 5 is therefore
//          the default; the louder half of the range stays reachable for
//          anyone who wants it.
inline const ParamSpec kParams[] = {
    {PARAM_SPEED, "Speed", "Speed", -5, 14, 5, false},
    {PARAM_PITCH, "Pitch", "Pitch", 0, 10, 5, false},
    {PARAM_VOLUME, "Volume", "Volume", 0, 9, 5, false},
    {PARAM_BRIGHTNESS, "Brightness", "Brightness", -30, 30, -5, false},
    {PARAM_EMPHASIS, "Emphasis", "Emphasis", 1, 99, 50, false},
    {PARAM_NASAL, "Nasal", "Nasal", 0, 1, 0, true},
    {PARAM_ORAL, "Oral", "Oral (head cold)", 0, 1, 0, true},
    {PARAM_BREATHY, "Breathy", "Breathy", 0, 1, 0, true},
    {PARAM_WHISPERY, "Whispery", "Whispery", 0, 1, 0, true},
    {PARAM_CREAKY, "Creaky", "Creaky", 0, 1, 0, true},
};
inline const int kParamCount = (int)(sizeof(kParams) / sizeof(kParams[0]));

// Engine-native parameter values, in the same order as kParams.
struct Params {
  int v[kParamCount];
};

inline Params default_params() {
  Params p;
  for (int i = 0; i < kParamCount; i++) p.v[i] = kParams[i].def;
  return p;
}

inline int clamp_param(int i, int value) {
  if (i < 0 || i >= kParamCount) return value;
  if (value < kParams[i].lo) return kParams[i].lo;
  if (value > kParams[i].hi) return kParams[i].hi;
  return value;
}

// --- Percent mapping ------------------------------------------------------
//
// The configuration utility presents every parameter as 0..100 %, where 0 % is
// the engine's minimum and 100 % its maximum, so the user never has to know the
// engine's asymmetric native ranges.
inline int percent_to_native(int i, int pct) {
  if (i < 0 || i >= kParamCount) return pct;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const ParamSpec &s = kParams[i];
  // Round to nearest so 100 % always reaches hi and 0 % always reaches lo.
  return s.lo + (int)(((long long)pct * (s.hi - s.lo) + 50) / 100);
}

inline int native_to_percent(int i, int native) {
  if (i < 0 || i >= kParamCount) return native;
  const ParamSpec &s = kParams[i];
  if (s.hi == s.lo) return 0;
  if (native < s.lo) native = s.lo;
  if (native > s.hi) native = s.hi;
  return (int)(((long long)(native - s.lo) * 100 + (s.hi - s.lo) / 2) /
               (s.hi - s.lo));
}

// --- Audio format ---------------------------------------------------------
inline const int kSampleRate = 11025;  // fixed by the engine
inline const int kBitsPerSample = 16;
inline const int kChannels = 1;

}  // namespace mono
