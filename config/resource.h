// resource.h - control ids for the Monologue '97 configuration utility.
//
// The numeric parameters are laid out so that parameter i of mono::kParams maps
// to a predictable pair of ids, and the boolean ones to a single checkbox id.

#pragma once

#define IDD_CONFIG 100
#define IDI_APPICON 101

#define IDC_VOICE 1001
#define IDC_VOICE_LABEL 1002

// Numeric parameters (kParams[0..4]): edit = IDC_NUM_EDIT + i*2,
// spin = IDC_NUM_EDIT + i*2 + 1.
#define IDC_NUM_EDIT 1010
#define IDC_NUM_FIRST 1010
#define IDC_NUM_LAST 1019

// Boolean parameters (kParams[5..9]).
#define IDC_CHK_FIRST 1030
#define IDC_CHK_LAST 1034

#define IDC_TESTTEXT 1040
#define IDC_TEST 1041
#define IDC_STOP 1042
#define IDC_DEFAULTS 1043
#define IDC_STATUS 1045
#define IDC_LOGS 1046
