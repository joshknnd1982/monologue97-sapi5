// mono_protocol.h - wire format between the SAPI5 dlls and mono_host.exe.
//
// The engine (mnvox11.dll) is a 32-bit PE, so a 64-bit host process -- 64-bit
// NVDA, Narrator, Balabolka -- cannot load it at all. Both the x86 and the x64
// SAPI dll therefore speak to the same 32-bit helper over this pipe. Routing
// x86 through the helper too costs one process hop but buys the thing that
// matters most here: a fault or a modal dialog inside a 1997 engine can no
// longer take down the screen reader hosting the voice.

#pragma once

#include <stdint.h>

#define MONO_PIPE_NAME L"\\\\.\\pipe\\Monologue97TTS"

// Bumped on any wire-format change. A helper left running by a previous
// install can still be serving when a new dll starts speaking -- the pipe name
// does not change between versions -- so the client checks this on connect and
// replaces a helper that does not match.
#define MONO_PROTOCOL_VERSION 1u

// Cancellation travels out of band. A blocking read and a write on the same
// synchronous pipe handle serialise against each other, so a client that is
// mid-read cannot also send a stop; instead it signals this per-session event
// and the helper checks it between PCM blocks.
#define MONO_CANCEL_EVENT_FMT L"Local\\Monologue97Cancel_%u"

enum MonoCommand : uint32_t {
  MONO_CMD_HELLO = 1,     // -> MonoHelloReply
  MONO_CMD_SPEAK = 2,     // MonoSpeakRequest + text -> audio stream
  MONO_CMD_SHUTDOWN = 3,  // helper exits
};

enum MonoReply : uint32_t {
  MONO_REP_HELLO = 1,
  MONO_REP_AUDIO = 2,  // MonoAudioChunk + PCM
  MONO_REP_END = 3,    // utterance complete (or cancelled)
  MONO_REP_ERROR = 4,  // MonoError
};

#pragma pack(push, 1)

struct MonoHeader {
  uint32_t type;
  uint32_t size;  // bytes following this header
};

struct MonoHelloReply {
  uint32_t protocol;
  uint32_t sample_rate;
  uint32_t bits;
  uint32_t channels;
  uint32_t font_count;
  uint32_t engine_version;  // SpeechVersion(), e.g. 0x010A
};

struct MonoSpeakRequest {
  char font[16];        // speech-font registry name, NUL padded
  int32_t params[16];   // engine-native values, in kParams order
  uint32_t param_count;
  uint32_t session;     // names the cancel event for this utterance
  uint32_t text_length; // bytes of ANSI text following this struct
};

struct MonoAudioChunk {
  uint32_t bytes;  // 16-bit mono PCM bytes following
};

struct MonoError {
  uint32_t length;  // bytes of ANSI message following
};

#pragma pack(pop)
