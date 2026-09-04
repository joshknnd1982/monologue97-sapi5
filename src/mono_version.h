// mono_version.h - single source of truth for the shipped version.
//
// Bump this for every build that leaves this machine. The number goes into
// every binary's version resource and into the installer's filename, so a
// released asset is never quietly replaced by a different build wearing the
// same name.

#pragma once

#define MONO_VER_MAJOR 1
#define MONO_VER_MINOR 1
#define MONO_VER_PATCH 0
#define MONO_VER_BUILD 0

#define MONO_VER_STRING "1.1.0"
#define MONO_VER_COMMA MONO_VER_MAJOR, MONO_VER_MINOR, MONO_VER_PATCH, MONO_VER_BUILD

#define MONO_PRODUCT "Monologue 97 SAPI5"
#define MONO_COMPANY "Josh Kennedy"
#define MONO_COPYRIGHT \
  "SAPI5 wrapper (c) 2026 Josh Kennedy. Monologue '97 engine (c) 1997 First Byte."
