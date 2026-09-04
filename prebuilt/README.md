# Prebuilt binaries

Everything here is built from the sources in this repository, so a clone gives
you working binaries without needing Visual Studio. All of them carry a version
resource matching the release they were cut from.

Most people should just run the installer from the
[Releases](https://github.com/joshknnd1982/monologue97-sapi5/releases) page —
it puts these files in place, registers the voices with SAPI and creates the
shortcuts. These copies are here for inspection, for scripted deployment, and
so the repository is self-contained.

## What ships

| File | |
| --- | --- |
| `monologue_sapi_x86.dll` | The SAPI5 engine for 32-bit hosts. |
| `x64/monologue_sapi_x64.dll` | The SAPI5 engine for 64-bit hosts. |
| `mono_host.exe` | The 32-bit helper that owns the speech engine. Both DLLs drive this one process. |
| `Monologue97Config.exe` | The configuration utility. |

These expect the speech engine in an `engine` folder beside them — the one in
[`../engine`](../engine). `mono_host.exe` and `Monologue97Config.exe` also look
one directory up, so the layout the installer creates works too.

## Registering by hand

Registration writes the SAPI voice tokens under `HKEY_LOCAL_MACHINE`, so it
needs an elevated command prompt. Run each DLL through the `regsvr32` of its own
architecture — the 32-bit DLL must land in the 32-bit COM view and the 64-bit
DLL in the native one:

```
%SystemRoot%\SysWOW64\regsvr32.exe monologue_sapi_x86.dll
%SystemRoot%\System32\regsvr32.exe x64\monologue_sapi_x64.dll
```

Add `/u` to unregister. There is also a per-user registration that needs no
elevation, used by the test tools; SAPI's voice category resolves to an HKLM
path, so voices registered that way work when bound by name but never appear in
a host's voice list.

## tools/

The verification programs, so the checks can be re-run without a compiler. Each
prints what it measured and exits non-zero on failure.

| Tool | What it proves |
| --- | --- |
| `prosody_test_x86/x64` | Pitch, rate and volume all move the audio — F0, duration and peak level measured from the render. |
| `spell_test_x86/x64` | Every character speaks during character-by-character navigation, the way NVDA drives it. |
| `sapi_test_x86/x64` | All 22 voices work through the real SAPI stack, and render *distinctly*. |
| `charfuzz` | No single byte 0x20–0xFF can kill the helper. |
| `a11y_check` | Every control in the configuration utility is reachable by Tab and announces a name (MSAA). |
| `bench`, `loopback_x86/x64` | Latency: engine and pipe timings, and WASAPI loopback timing of what actually reaches the speakers. |
| `make_samples` | Re-renders the WAVs in [`../samples`](../samples). |

Run them with no arguments for usage. The SAPI-driven ones register the DLL
per-user for the duration of the run and unregister afterwards.
