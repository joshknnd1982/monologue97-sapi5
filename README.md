# Monologue 97 SAPI5

A SAPI5 text-to-speech voice built on **Monologue '97**, the 1997 First Byte /
PrimoVOX speech synthesizer. It publishes all **21 of the engine's voices** plus
a user-configurable **Custom Voice** to every SAPI5 application on modern
Windows — NVDA, Narrator, Balabolka, and anything else that speaks — in both
**32-bit and 64-bit** hosts.

The original product was a Windows 95/NT program with a SAPI4 interface. This
project skips SAPI4 entirely and drives the engine's native C API directly.

---

## What Monologue '97 actually is

First Byte (a division of Davidson & Associates) shipped a family of formant
synthesizers through the late 80s and 90s — SmoothTalker, ProVoice, Monologue.
Monologue '97 version 3.0 was the last Windows release, dated June 1997, and its
speech technology was covered by US patents 4,692,941, 4,617,645 and 4,805,220,
all long since expired.

The interesting part is the shape of the product:

| File | What it is |
| --- | --- |
| `mnvox11.dll` | The engine. A native 32-bit PE with a 54-function `__stdcall` C API. Reports version 1.10. |
| `Pvoxa.dll` | A pure jump-thunk forwarder to `mnvox11.dll`. No logic of its own. |
| `SAPIPMV.DLL` | The SAPI4 TTS layer, which imports from `Pvoxa.dll`. |
| `SF\*.DLL` | The voices. Resource-only DLLs — no code. |

Because SAPI4 sits *on top* of a documented-enough C API, it can be removed from
the picture completely. **This project does not use, require, or install any
SAPI4 component.** `SAPIPMV.DLL` and `Pvoxa.dll` are not shipped.

> There is a second, unrelated thing also called "Monologue": an NVDA add-on that
> emulates a *16-bit* First Byte engine (`FB_SPCH.DLL` / `FB_NGN.EXE`) under the
> Unicorn CPU emulator, and exposes two voices. This project is not that. The CD
> engine is native 32-bit code, needs no emulation, and has 21 voices.

### Languages

**US English only.** This is worth stating plainly because the engine binary is
misleading: `mnvox11.dll` contains strings for dozens of languages and countries
— `french-canadian`, `spanish-mexican`, `chinese-traditional`, and so on. Those
are **Microsoft C runtime locale tables**, not engine capabilities. They are
linked in by `setlocale` and have nothing to do with speech.

The shipped voice data (`ENMH.DLL`, `ENFH.DLL`) is US English, the dictionaries
are US English, and there are no other language modules. All voice tokens are
registered under LCID 409.

---

## The 21 voices

Two of the speech fonts are *base* fonts carrying the full synthesis data set
(timing rules, tagger rules, mu-law tables, formant data, dictionaries). The
other 19 are ~6 KB *modifier* fonts: a `BASELINE` resource pointing at a base
font, plus prosody overrides.

| Voice | Speech font | Base | Gender |
| --- | --- | --- | --- |
| Monologue US Male | `ENMH` | yes | Male |
| Monologue US Female | `ENFH` | yes | Female |
| Monologue Male Breathy | `BREATHY` | | Male |
| Monologue Male Deep | `DEEP` | | Male |
| Monologue Male Deeper | `DEEPER` | | Male |
| Monologue Male Helium | `HELIUM` | | Male |
| Monologue Male Impediment | `NASAL` | | Male |
| Monologue Male Head Cold | `ORAL` | | Male |
| Monologue Male Soft | `SOFT` | | Male |
| Monologue Male Chime | `SPACE` | | Male |
| Monologue Male Very High | `VERYHIGH` | | Male |
| Monologue Male Whisper | `WHISPER` | | Male |
| Monologue Female Breathy | `FBREATHY` | | Female |
| Monologue Female Deep | `FDEEP` | | Female |
| Monologue Female Gravelly | `FDEEPER` | | Female |
| Monologue Female Higher | `FHIGHER` | | Female |
| Monologue Female Head Cold | `FORAL` | | Female |
| Monologue Female Smokey | `FSMOKEY` | | Female |
| Monologue Female Whisper | `FWHISPER` | | Female |
| Monologue Monotone | `MONOTONE` | | Male |
| Monologue Xylon Robot | `XYLON` | | Male |

Plus **Monologue 97 Custom Voice**, whose speech font and every parameter come
from whatever the configuration utility last saved.

Output is **11025 Hz, 16-bit mono** — fixed by the engine.

---

## Speech parameters

The engine's `SetSpeechParameter` takes IDs 0–34, but only ten of them are real
parameters. The rest are internal control-block slots (notification window and
message pairs, buffer pointers, the sample rate at ID 20, the format flag at 21)
and writing to them corrupts the engine.

Every parameter is presented in the configuration utility as **0 %–100 %**,
where 0 % is the engine's minimum and 100 % its maximum.

| Parameter | ID | Engine range | Default | What it does |
| --- | --- | --- | --- | --- |
| Speed | 2 | −5 … **14** | 5 | Linear duration scale. Higher is faster. |
| Pitch | 1 | −5 … 15 | 5 | Fundamental frequency. Measured 77 Hz at 0 %, 119 Hz at 50 %, 200 Hz at 100 %. |
| Volume | 0 | 0 … 9 | 5 | Output level. 0 is silent. |
| Brightness | 23 | −30 … 30 | −5 | Spectral tilt / vocal-tract brightness. |
| Emphasis | 24 | 1 … 99 | 50 | Prosodic range — how much pitch moves across a phrase. |
| Nasal | 25 | 0 / 1 | off | Nasalised voice quality. |
| Oral | 26 | 0 / 1 | off | "Head cold" quality. |
| Breathy | 27 | 0 / 1 | off | Adds breath noise. |
| Whispery | 28 | 0 / 1 | off | Whispered voice. |
| Creaky | 29 | 0 / 1 | off | Creaky / vocal-fry quality. |

Two ranges are deliberately narrower than what the engine will accept:

- **Speed maxes out at 14, not 15.** Speed is a linear duration scale that
  reaches exactly zero at 15: the engine accepts the value and then renders
  *nothing at all*. Publishing 15 as "100 %" would let a user silence their own
  screen reader.
- **Volume defaults to 5, not 9.** The engine's output is already hot — at
  Volume 9 roughly 14 % of samples clip, while 5 is clean. The louder half of
  the range stays reachable for anyone who wants it.

SAPI's own rate and volume still work on top of all this: a host's rate slider
shifts Speed around the configured baseline, and a host's volume is applied as
software gain (smooth, and it never clips, unlike the engine's ten-step Volume).

---

## The configuration utility

**Monologue 97 Speech Settings** edits the Custom Voice: its speech font and all
ten parameters. The SAPI Custom Voice re-reads the saved snapshot for *every
utterance*, so a change takes effect on the next thing spoken — no restart of
NVDA or of the speaking application.

It is built as a screen-reader tool first:

- Numbers are **edit boxes with spin buttons, never trackbars**. MSAA reports a
  trackbar's position as a percentage of its own range, so a slider showing 5
  gets announced as "55". An edit box announces the digits actually shown.
- Every control is preceded in the tab order by its own label, which is where
  MSAA takes an edit box's accessible name from.
- Every control is in the tab order and carries an access key.
- Verified with an MSAA walker (`tools/a11y_check.cpp`), not by eye: 18 controls
  in the tab ring, 18 with accessible names.

`Speak test` renders through the real engine and plays it; `Open log folder`
opens this user's log directory.

---

## How it is put together

```
SAPI5 host (32- or 64-bit)
        |  COM
monologue_sapi_x86.dll / monologue_sapi_x64.dll     ISpTTSEngine
        |  named pipe  \\.\pipe\Monologue97TTS
mono_host.exe (32-bit)                              owns the engine
        |  direct calls
engine\mnvox11.dll + engine\SF\*.DLL                the 1997 engine
```

`mnvox11.dll` is a 32-bit PE, so a 64-bit host cannot load it at all. Both SAPI
DLLs therefore talk to one 32-bit helper. Routing the 32-bit build through the
helper as well costs one process hop but buys the thing that matters most for a
screen-reader voice: **a fault or a modal dialog inside 1997 code can no longer
take down the screen reader hosting it.**

### Latency

Measured with `QueryPerformanceCounter` and with WASAPI loopback capture of the
real audio endpoint — SAPI's own `SPEI_START_INPUT_STREAM` event fires when SAPI
starts consuming the stream, not when a sample reaches the speakers, and
under-reports by around 60 ms.

| Stage | Cost |
| --- | --- |
| `TextToCmd` + first PCM block | 0.5–0.9 ms |
| Whole utterance rendered (one character) | ~1 ms |
| Pipe round trip to the helper | ~0.5 ms |
| `ISpTTSEngine::Speak` returns | ~2 ms |
| **Keypress to audible sound (x86 and x64)** | **~90 ms mean** |

Everything this project controls adds up to about 4 ms; the remaining ~86 ms is
SAPI's audio object and the Windows audio stack, which the engine never sees.

The one real saving available was the engine's own padding. Every utterance
arrives with roughly 55 ms of silence before the first phoneme and 40–55 ms
after the last — invisible in a sentence, but more than half the audio when a
screen reader speaks one character per keystroke, and paid again on every
keypress while arrowing. A silence gate drops the leading run and withholds the
trailing one (silence *between* words is kept, and only discarded if the
utterance ends there):

| Utterance | Before | After |
| --- | --- | --- |
| `"a"` | 190 ms | 110 ms |
| `"hello"` | 449 ms | 309 ms |
| A full sentence | 2594 ms | 2504 ms |

Two things were measured and deliberately **not** shipped: offering SAPI 22050
or 44100 Hz and upsampling in the wrapper, in case 11025 Hz was awkward for a
48 kHz endpoint (no difference — 88–94 ms mean either way across repeated runs),
and any form of output-rate conversion in general. The helper and its engine
thread do run above normal priority, since they sit between a keypress and a
sound.

`tools/bench.cpp`, `tools/loopback.cpp` and `tools/charfuzz.cpp` reproduce all
of these numbers.

### Notes from reverse-engineering the engine

Things that are not obvious and cost real time to find:

- **`OpenSpeech`'s first argument is an `HWND`, not an `HINSTANCE`.** It lands in
  four (window, message) notification slots, and `Say` spins on
  `PeekMessage(hwnd, …)` waiting for a completion message. Pass a module handle
  and `Say` spins forever.
- **Startup is registry-gated.** `OpenSpeech` enumerates
  `HKLM\Software\FirstByte\PrimoVOX\SpeechFonts`, reads its `Path` value for the
  font directory, and treats each subkey `NAME` as `<Path>\NAME.DLL`. This
  project never writes there: it points `HKEY_LOCAL_MACHINE` at a private
  volatile key with `RegOverridePredefKey` for the duration of the call, then
  takes the alias straight back down. No administrator rights, real HKLM
  untouched.
- **Each font subkey must have a `Description` value.** The engine
  `RegQueryValueEx`es it into a buffer it never initialises, so a missing one
  makes `OpenSpeech` fault outright.
- **Audio must be pulled, not played.** `Say` with no backend plays to the sound
  card and hands the caller nothing; `OpenBackend` (a WAV file) plus `Say`
  deadlocks. The usable path is
  `TextToCmd` → `OpenBackendCmd` → `GetPCMdata` → `CloseBackend`: synchronous,
  pull-based, no audio device needed, and cancellable between blocks.
- The engine reports failures with **modal message boxes**. Its import of
  `USER32!MessageBoxA` is redirected to a stub that logs the text and answers
  OK — a modal dialog inside a SAPI host would hang the screen reader.

---

## Installing

Run `Monologue97-SAPI5-Setup-<version>.exe` from the
[Releases](https://github.com/joshknnd1982/monologue97-sapi5/releases) page. It
needs administrator rights: SAPI's voice category resolves to an HKLM path, so a
per-user registration would install cleanly and then never be enumerated by any
host.

The installer offers a **desktop shortcut** for the configuration utility, and
registers the 32-bit DLL into the 32-bit COM/SAPI view and the 64-bit DLL into
the native one, so 32-bit and 64-bit applications both see the full voice list.

Uninstalling unregisters both and removes the engine.

### Logs

Everything writes a detailed log to:

```
%LOCALAPPDATA%\Monologue97\logs\
```

one file per component per process — `sapi-x64-1234.log`, `host-5678.log`,
`config-9012.log`. The installer's own log is copied to the install directory as
`install-<version>.log`. The configuration utility has an **Open log folder**
button.

---

## Building from source

Requires Visual Studio 2022 Build Tools (C++, x86 and x64), CMake, and
Inno Setup 6.

```
build_all.bat
```

builds both architectures, stages `output\`, and compiles the installer into
`dist\`.

**The engine files are not in this repository.** `mnvox11.dll`, `SF\*.DLL` and
the dictionaries are First Byte's copyrighted work, redistributed only in the
Releases installer. To build a working installer yourself, populate `engine\`
from a Monologue '97 CD:

```
engine\mnvox11.dll          from  TTS\mnvox11.dll
engine\SF\*.DLL             from  SF\
engine\SF\ENPBKERN.DIC      from  SF\
engine\USA.DIC              from  the CD root
engine\CDSF.INI             from  the CD root
```

### Verifying

- `make_samples.exe <engine-dir> <out-dir>` — renders every voice and every
  parameter extreme to WAV, and fails on a silent render.
- `sapi_test_x86.exe` / `sapi_test_x64.exe` — registers the DLL per-user, drives
  the **real SAPI stack** through every voice, and fails if two voices render
  identically (which is what happens when the token's speech font never reaches
  the engine).
- `a11y_check.exe` — walks the configuration utility's MSAA tree and fails if any
  control is unreachable by Tab or has no accessible name.

---

## Licence

The wrapper is MIT. The engine files are Copyright © 1997 First Byte and are
redistributed as abandonware for preservation and accessibility use. See
[LICENSE.txt](LICENSE.txt) — including how to ask for their removal if you hold
those rights.
