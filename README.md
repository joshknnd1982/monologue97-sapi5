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
| Pitch | 1 | **0 … 10** | 5 | Fundamental frequency. Measured 77 Hz at 0 %, 119 Hz at 50 %, 200 Hz at 100 %. |
| Volume | 0 | 0 … 9 | 5 | Output level. 0 is silent. |
| Brightness | 23 | −30 … 30 | −5 | Spectral tilt / vocal-tract brightness. |
| Emphasis | 24 | 1 … 99 | 50 | Prosodic range — how much pitch moves across a phrase. |
| Nasal | 25 | 0 / 1 | off | Nasalised voice quality. |
| Oral | 26 | 0 / 1 | off | "Head cold" quality. |
| Breathy | 27 | 0 / 1 | off | Adds breath noise. |
| Whispery | 28 | 0 / 1 | off | Whispered voice. |
| Creaky | 29 | 0 / 1 | off | Creaky / vocal-fry quality. |

Two ranges are deliberately narrower than what `SetSpeechParameter` will accept,
because part of what it accepts does nothing — or worse:

- **Speed maxes out at 14, not 15.** Speed is a linear duration scale that
  reaches exactly zero at 15: the engine accepts the value and then renders
  *nothing at all*. Publishing 15 as "100 %" would let a user silence their own
  screen reader.
- **Pitch is 0–10, not −5–15.** Measuring F0 across the accepted range shows the
  engine only responds between 0 and 10. Every value at or below 0 renders
  byte-identically at 76 Hz, and 10 upwards all sit at 200 Hz. Publishing the
  accepted range left half of a host's pitch slider inert.
- **Volume defaults to 5, not 9.** The engine's output is already hot — at
  Volume 9 roughly 14 % of samples clip, while 5 is clean. The louder half of
  the range stays reachable for anyone who wants it.

### How the host's rate, pitch and volume arrive

Worth knowing if you are writing a SAPI5 engine of your own: **SAPI has no
`ISpTTSEngineSite::GetPitch`.** Rate and volume have voice-level getters, but
pitch is delivered *only* per fragment, as `SPVTEXTFRAG::State.PitchAdj.MiddleAdj`,
set from a `<pitch absmiddle="N">` tag — which is exactly how NVDA adjusts it.
An engine that reads `GetRate`/`GetVolume` and never looks at the fragment state
will move rate and volume perfectly and ignore pitch completely.

All three are therefore read per fragment and combined with the voice's base
settings:

| Control | Voice-level base | Per-fragment | Combined as |
| --- | --- | --- | --- |
| Rate | `GetRate()` −10…10 | `State.RateAdj` −10…10 | sum, spread over Speed's range |
| Pitch | *(none exists)* | `State.PitchAdj.MiddleAdj` −10…10 | spread over Pitch's range |
| Volume | `GetVolume()` 0…100 | `State.Volume` 0…100 | multiplied, applied as software gain |

Volume is applied as gain here rather than through the engine's own Volume
parameter, which has only ten steps and clips across the top half.

`tools/prosody_test.cpp` measures all three from the rendered audio — F0 for
pitch, duration for rate, peak for volume — so a control that silently stops
working cannot pass.

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

### Reading one character at a time

Two separate things have to be right for arrowing character by character to
work, and both are easy to get wrong in a way that no ordinary test notices.

**NVDA does not send character navigation as plain speech.** It wraps the
character in `<spell>…</spell>` with `SPF_IS_XML`, which SAPI delivers as a text
fragment whose `State.eAction` is `SPVA_SpellOut` — *not* `SPVA_Speak`. An
engine that skips everything except `SPVA_Speak` (a reasonable-looking way to
avoid speaking bookmark fragments aloud) silently drops every letter and every
punctuation mark the user arrows over, while sentences keep working perfectly.

**Twenty printable characters render as pure silence in this engine:**

```
space  !  "  '  (  )  ,  -  .  /  :  ;  ?  [  \  ]  ^  {  |  }
```

Handing any of those to the engine produces zero samples, so every ASCII symbol
is given an explicit spoken name in the SAPI layer instead — `.` becomes "dot",
`,` "comma", `-` "dash", `(` "left paren", and so on. That also makes the
announcement identical whatever symbol level the screen reader is set to.

One case genuinely cannot be fixed here: a **whitespace-only utterance** is
stripped by SAPI itself, which hands the engine a fragment of length zero, so no
SAPI5 voice can speak it. NVDA does not depend on that — it substitutes the word
"space" before speaking.

`tools/spell_test.cpp` drives the real SAPI stack exactly as NVDA does and fails
if any character produces no audio.

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

## What is in this repository

A clone gives you everything — sources, the speech engine, working binaries and
the rendered samples. Nothing has to be downloaded separately except the
installer itself.

| | |
| --- | --- |
| [`src/`](src) | The SAPI5 engine, the 32-bit helper and the shared engine wrapper. |
| [`config/`](config) | The configuration utility. |
| [`engine/`](engine) | The 1997 speech engine: `mnvox11.dll`, the 21 speech fonts in `SF\`, and the dictionaries. |
| [`prebuilt/`](prebuilt) | Ready-to-run binaries for both architectures, plus the verification tools. See [prebuilt/README.md](prebuilt/README.md). |
| [`samples/`](samples) | 49 rendered WAVs: every voice, every parameter at 0/50/100 %, and combinations. |
| [`tools/`](tools) | Verification and measurement sources, and the reverse-engineering probes. |
| [`installer/`](installer) | The Inno Setup script. |

The compiled installer is not in the repository — it is published on the
[Releases](https://github.com/joshknnd1982/monologue97-sapi5/releases) page.
Neither is the original Monologue '97 CD image: `SETUP.EXE`, the
electronic-registration tool and the compressed archives are not needed to build
or run anything here, and `engine/` already holds the parts that are.

## Building from source

Requires Visual Studio 2022 Build Tools (C++, x86 and x64), CMake, and
Inno Setup 6.

```
build_all.bat
```

builds both architectures, stages `output\`, and compiles the installer into
`dist\`. The engine files it needs are already in `engine\`.

### Verifying

Prebuilt copies of all of these are in [`prebuilt/tools`](prebuilt/tools), so
the checks can be re-run without a compiler. Each exits non-zero on failure.

- `prosody_test` — sweeps pitch, rate and volume through the real SAPI stack and
  measures the **rendered audio**: F0 for pitch, duration for rate, peak for
  volume. Pitch is swept one step at a time, because a coarse check cannot tell
  "works but saturates" apart from "does nothing".
- `spell_test` — drives character-by-character navigation the way NVDA does
  (`<spell>` markup, which arrives as `SPVA_SpellOut`) and fails if any
  character is silent.
- `sapi_test` — drives every voice through real SAPI, and fails if two voices
  render *identically* — which is what happens when a token's speech font never
  reaches the engine, while every voice still "works".
- `charfuzz` — speaks every byte 0x20–0xFF through the helper to prove none of
  them can kill it. A helper crash would silently cost a process relaunch on the
  next keystroke, which is exactly what intermittent lag looks like.
- `a11y_check` — walks the configuration utility's MSAA tree and fails if any
  control is unreachable by Tab or has no accessible name.
- `bench`, `loopback` — latency, the second via WASAPI loopback capture of the
  real endpoint.
- `make_samples` — renders every voice and parameter extreme, failing on a
  silent render.

## Version history

### 1.4.0 — capital letters, and binaries in the repository

Arrowing across capitals announced the wrong thing, and only for some letters:
`C` was read as "one hundred", `D` as "five hundred", `L` "fifty", `M` "one
thousand", `V` "five". Those are exactly the Roman numerals — the engine's text
analysis reads a lone capital numeral as a *number*. The lower-case form of
every letter is spoken as the letter, so single letters are now lower-cased on
the way to the engine. Nothing is lost: a screen reader indicates capitals
itself, NVDA by raising the pitch for that fragment.

Measured across all 26 letters, upper against lower: `D` was 3.26× longer before
the fix, `C` 2.65×, `M` 2.36×, `L` 1.83×, `V` 1.52×. All 26 are now identical.

This release also puts the engine, the built binaries and the rendered samples
in the repository, so a clone is self-contained.

### 1.3.0 — pitch

Pitch did nothing when adjusted from NVDA or any other SAPI5 application. SAPI
has no `ISpTTSEngineSite::GetPitch`: pitch arrives *only* per fragment as
`State.PitchAdj.MiddleAdj`, so reading the rate and volume getters moved those
two perfectly and ignored pitch entirely. `<rate>` and `<volume>` tags were
being dropped for the same reason — invisible under NVDA, which sets those
through the voice properties, but broken for other hosts.

Measuring then showed a second problem: pitch was published as the range the
engine *accepts* (−5…15), but it only *responds* between 0 and 10, leaving half
of a host's slider inert. Now 0–10, so the whole slider works.

*Before: 118.5 Hz at every pitch setting. After: 76 → 200 Hz over 11 distinct
levels.*

### 1.2.0 — character navigation

Arrowing character by character said nothing at all, while sentences worked
perfectly. NVDA wraps character navigation in `<spell>…</spell>`, which SAPI
delivers as `SPVA_SpellOut` rather than `SPVA_Speak`; the engine skipped
everything that was not `SPVA_Speak` and so dropped every letter and punctuation
mark. Separately, twenty printable characters render as pure silence in this
engine, so every ASCII symbol now gets an explicit spoken name.

Also fixed a test that had been hiding this: the harness assumed WAV audio
starts at byte 44, but SAPI writes an 18-byte `fmt` chunk, so a silent render
was reporting a healthy peak and passing.

### 1.1.0 — latency

Every utterance carries ~55 ms of silence before the first phoneme and 40–55 ms
after the last. Invisible in a sentence; more than half the audio when a screen
reader speaks one character per keystroke. A silence gate now trims the padding
while keeping pauses between words — `"a"` went from 190 ms to 110 ms. The
helper and its engine thread also run above normal priority.

Measured with QPC and WASAPI loopback: keypress to audible sound is ~90 ms, of
which about 4 ms belongs to this project and the rest to the Windows audio
stack.

### 1.0.0 — first release

21 voices plus a configurable Custom Voice, 32- and 64-bit, no SAPI4.

---

## Licence

The wrapper — everything under `src/`, `config/`, `tools/` and `installer/` — is
MIT.

The engine files in [`engine/`](engine) are **not** covered by that and are not
this project's work. They are Monologue '97 (PrimoVOX), Copyright © 1997 First
Byte, a division of Davidson & Associates, redistributed as abandonware for
preservation and accessibility use: the product has been unsold and unsupported
for decades, its speech patents expired long ago, and no rights holder has been
locatable.

See [LICENSE.txt](LICENSE.txt) for the full terms — including how to ask for the
engine files to be removed if you hold those rights.
