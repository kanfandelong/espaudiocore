# espaudiocore

ESP-IDF-native **unified multi-decoder wrapper library**: one call starts playback, decoding runs in a background task, and the decode task is managed internally by the library.

- Target framework: **ESP-IDF v6.1+** (validated on ESP32-S3 hardware)
- Public API reference: [**API.md**](API.md)

---

## Features

- **7 formats**: WAV / MP3 / AAC / FLAC / OGG Vorbis / Opus / WavPack
- **Automatic format detection**: file input is routed in two stages (`extension -> magic number`), with support for skipping an ID3v2 prefix
- **Two input types**: local files (absolute paths including the mount point) and ringbuf (network streams / upstream decoders)
- **Two output types**: I2S (the library handles sample-rate changes internally) and ringbuf (the application retrieves PCM itself)
- **High-resolution preservation**: 24/32-bit sources are not downconverted to 16-bit
- **Zero-copy metadata**: large data such as cover art does not consume extra heap and is not copied
- **Optional PSRAM**: large buffers prefer PSRAM, with automatic fallback to internal RAM

---

## Supported formats

| Format | Extension | Decoder library | File input | ringbuf input | High bit depth |
| --- | --- | --- | --- | --- | --- |
| WAV | `.wav` | Built-in | ✅ | ✅ | 8/16/24/32-bit, float32 |
| MP3 | `.mp3` | libhelix-mp3 | ✅ | ✅ | — |
| AAC | `.aac` | libhelix-aac (with SBR) | ✅ | ✅ | — |
| FLAC | `.flac` | libFLAC | ✅ | ✅ | 16/24-bit |
| OGG Vorbis | `.ogg` | libvorbis + libogg | ✅ | ❌ | — |
| Opus | `.opus` | libopus + opusfile | ✅ | ❌ | — |
| WavPack | `.wv / .wvc` | libwavpack | ✅ | ✅ | 16/24, float32 |

> ringbuf cannot be used for Vorbis / Opus: their container libraries require a seekable source, so the call returns
> `ESP_ERR_NOT_SUPPORTED` instead of failing silently.

---

## Quick start

```c
#include "espaudiocore.h"

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    if (ev == ESPAUDIOCORE_EVT_EOS) {
        /* Playback finished. This callback runs on the decode task; do not block. */
    }
}

void app_main(void)
{
    /* The I2S channel is created by the application: GPIO / DMA / init_std_mode / enable are all on the application side */
    i2s_chan_handle_t tx = my_i2s_init();

    espaudiocore_cfg_t cfg = {
        .output_bits = 32,     /* PCM5102: fixed 32-bit slot width */
        .volume_db   = -6.0f,
        .on_event    = on_event,
    };

    int dec_err = 0;
    esp_err_t err = espaudiocore_begin("/sdcard/a.mp3", tx, &cfg, &dec_err);
    if (err != ESP_OK) {
        /* err is an IDF semantic code; dec_err is the decoder library's native code */
        ESP_LOGE(TAG, "begin: %s (dec_err=%d)", esp_err_to_name(err), dec_err);
    }
}
```

Key points:

- `begin()` **returns immediately**; it starts an internal decode task, so the application does not need to poll.
- The path must be an **absolute path including the mount point** (`/sdcard/xxx`).
- When the output is ringbuf, sample-rate handling is the application's responsibility (read `espaudiocore_get_format()`).
- Errors are split into two layers: `esp_err_t` follows IDF semantics; the decoder library's native code is returned via the `dec_err` out parameter.

---

## Four decoding modes

| Input | Output | Function | Typical scenario |
| --- | --- | --- | --- |
| File | I2S | `espaudiocore_begin()` | Local player |
| File | ringbuf | `espaudiocore_begin_rb()` | Transcoding, custom output |
| ringbuf | I2S | `espaudiocore_begin_stream()` | Internet radio |
| ringbuf | ringbuf | `espaudiocore_begin_stream_rb()` | Streaming pipeline |

See [API.md](API.md) for parameter and return value details.

---

## Adding to a project

Place `espaudiocore/` into the project's `components/` directory. There are no external dependencies (the decoder libraries are bundled with the component).

```cmake
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES espaudiocore)
```

---

## Configuration

`idf.py menuconfig -> Component config -> espaudiocore`

| Group | Option | Default | Description |
| --- | --- | --- | --- |
| — | `LOG_LEVEL` | 3 (INFO) | Can be overridden at runtime by `esp_log_level_set("espaudiocore", ...)` |
| **Decoders** | `ENABLE_WAV` / `MP3` / `AAC` / `FLAC` / `VORBIS` / `OPUS` | All enabled | Disabling can significantly reduce firmware size; Opus has high memory usage and is recommended to be disabled without PSRAM |
| **Output** | `OUTPUT_BITS` | 0 | `0` = follow PCM; `16/24/32` = fixed width (`cfg.output_bits` takes precedence) |
| | `DEFAULT_VOLUME_DB` | 0 | Only used as the default when `cfg.volume_db` is not set |
| **Memory** | `FILE_SOURCE_MODE` | Pure VFS + setvbuf | File read strategy: pure VFS buffering, or a self-maintained read-ahead window |
| | `SOURCE_BUFFER_SIZE` | 32768 | Self-managed window size |
| | `VFS_BUFFER_SIZE` | 65536 | `setvbuf` buffer size in pure VFS mode |
| | `PSRAM_REQUIRED` | n | When enabled, a PSRAM allocation failure reports an error directly and does not fall back to internal RAM |
| **Task** | `TASK_STACK_DEFAULT` | 8192 | Fallback stack size; each decoder can provide a recommended value in the routing table (16384 for Opus) |
| | `TASK_PRIORITY` | 8 | Decode task priority |
| | `TASK_CORE` | 1 | Decode task core affinity |
| — | `FLAC_XTENSA_ASM` | n | FLAC LPC assembly for ESP32-S3; requires also enabling `EN_ESP_ASM` in `lib/libflac/config.h`; the effect is not significant (xtensa cores only) |

Output bit width and volume can also be set at runtime via `espaudiocore_cfg_t`:

- `output_bits`: `0` follows PCM; `16/24/32` fixes the width. Narrower sources are left-shifted/padded, and wider sources are right-shifted/truncated.
- `volume_db`: integer scaling with saturation; at `0 dB`, no multiply-add is performed.

---

## Known limitations

| Limitation | Description |
| --- | --- |
| ringbuf input does not support seek | Some decoder libraries may not work correctly with streaming input |
| ringbuf input cannot detect format | The encoding format `espaudiocore_format_t` must be passed explicitly |
| M4A input limitations | File mode supports avpack + Helix AAC-LC; requires seekable input, ringbuf unsupported; the AAC decoder must be enabled |
| Single session at a time | Call `stop()` before calling `begin*()` again |

---

## License

Component's own code: **MIT**.

Third-party decoder libraries are kept as-is in principle, with only necessary minimal adaptations; see [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) for licenses.