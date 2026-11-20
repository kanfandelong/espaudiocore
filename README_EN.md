# espaudiocore — native Linux branch (`linux-native`)

A pure Linux implementation of the multi-format audio decoder library: pthread
decode worker, POSIX file/ring-buffer I/O, ALSA output. No ESP-IDF, FreeRTOS,
I2S or "pretend the ESP API is Linux" shim layer.

> `master` remains the ESP-IDF component (validated on ESP32-S3). The two
> branches have different public APIs. This branch targets RK3506 / ARMv7
> Linux boards and desktop Linux; do not merge it back into `master`.

---

## What changed vs `master`

| Area | `master` (ESP-IDF) | `linux-native` |
|---|---|---|
| Build | ESP-IDF component + Kconfig | plain CMake + options |
| Output | app passes `i2s_chan_handle_t` | library opens ALSA (`cfg.alsa_device`) |
| Returns | `esp_err_t` | `int`, 0 or negative errno |
| Threads | FreeRTOS tasks | `pthread_create` / `pthread_join` |
| Sync | `xSemaphore` / `volatile bool` | `pthread_mutex`, `std::atomic`, condvars |
| Ring buffer | FreeRTOS `RingbufHandle_t` | built-in `espaudiocore_ringbuf_t` |
| Logs | `ESP_LOGx` | stderr + level filter |
| Memory | `heap_caps_*` (PSRAM/DMA) | `posix_memalign` / `malloc` |
| I2S | `driver/i2s_std.h` | ALSA via `dlopen("libasound.so.2")` |
| Paths | absolute only | relative or absolute |

The old `port/linux/` shim headers (`esp_err.h`, `esp_log.h`,
`esp_heap_caps.h`, `sdkconfig.h`, `freertos/*`, `driver/i2s_std.h`) and
`linux_port.cpp` are gone; nothing in this branch needs them.

---

## Build

```bash
./scripts/build.sh                 # host build
./scripts/build.sh --cross         # RK3506 ARMv7 cross build
```

Outputs:

```
~/espaudiocore-linux/build-host/{eac_play,libespaudiocore.a}
~/espaudiocore-linux/build-arm/eac_play
```

Manual build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requirements: Linux/WSL, CMake >= 3.16, GCC >= 8 with C++17. Runtime needs
`libasound.so.2`; ALSA development headers are **not** needed because the
library loads ALSA dynamically.

Decoder selection:

```bash
cmake -S . -B build -DESPAUDIOCORE_ENABLE_OPUS=OFF -DESPAUDIOCORE_ENABLE_WAVPACK=OFF
```

---

## Quick start

```c
#include "espaudiocore.h"

static void on_event(void *user, espaudiocore_event_t ev, const char *msg) {
    (void)user;
    fprintf(stderr, "event %d: %s\n", (int)ev, msg);
}

int main(void) {
    espaudiocore_cfg_t cfg = {0};
    cfg.alsa_device = "plughw:0,0";   /* NULL => "default" */
    cfg.on_event = on_event;

    int dec_err = 0;
    int rc = espaudiocore_begin("/music/a.flac", &cfg, &dec_err);
    if (rc != 0) {
        fprintf(stderr, "%s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
        return 1;
    }
    while (espaudiocore_is_running())
        usleep(100000);
    espaudiocore_stop();
    return 0;
}
```

Link with `-lespaudiocore -lpthread -ldl -lm`.

CLI:

```bash
eac_play song.mp3
eac_play song.flac --device plughw:0,0
eac_play song.ogg --raw-out /tmp/out.pcm --bits 16
```

On the board `/dev/snd` is usually root-only; run `eac_play` with `sudo` or add
the user to the `audio` group.

---

## API summary

| Function | Purpose |
|---|---|
| `espaudiocore_begin(path, cfg, dec_err)` | file → ALSA |
| `espaudiocore_begin_rb(path, rb, cfg, dec_err)` | file → ring buffer |
| `espaudiocore_begin_stream(rb, fmt, cfg, dec_err)` | compressed ring → ALSA |
| `espaudiocore_begin_stream_rb(rb, fmt, out, cfg, dec_err)` | ring → ring |
| `espaudiocore_stop/pause/resume/seek_ms` | transport |
| `espaudiocore_set_volume_db/get_volume_db` | software volume |
| `espaudiocore_get_position_ms/duration_ms/get_format` | queries |
| `espaudiocore_ringbuf_*` | thread-safe byte ring buffer |

Errors are negative errno values; `espaudiocore_strerror()` prints them.
Callbacks run on the decoder thread and must not block or call `stop()`.

---

## Verification

| Test | Result |
|---|---|
| Host + ALSA `null`, all 8 formats | STARTED → EOS → STOPPED |
| RK3506 + ALSA `null`, all 8 formats | pass |
| RK3506 + `plughw:0,0`: WAV / Opus 48k / M4A / 24-bit | opens, configures, real-time pacing, clean stop |
| Ring-buffer PCM vs ffmpeg: WAV16, FLAC16, WAV→24 | byte-identical |
| `eac_selftest`: pause freeze, resume, seek, EOS, stop | pass |

```bash
./tests/smoke.sh ~/espaudiocore-linux/build-host/eac_play
./tests/compare_pcm.sh ~/espaudiocore-linux/build-host/eac_play tests/assets
./scripts/board_test.sh --alsa
```

---

## Limitations

- Linux only; ESP32 stays on `master`.
- ALSA is the only native backend (PulseAudio/PipeWire work through ALSA plugins).
- Software volume only, no hardware mixer control.
- `pause()` falls back to "stop feeding" when `snd_pcm_pause` is unsupported.
- Opus/Vorbis cannot be used as **ring-buffer input** (container needs seeking).
- Third-party patches: libogg allocator, libFLAC allocator config, helix platform
  branches, plus `compat/pgmspace.h` + `compat/Arduino.h` for the vendored AVR-era
  headers. See `README.md` section 10 for details.

Licenses: MIT for this project; see `THIRD_PARTY_LICENSES`.
