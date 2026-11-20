# espaudiocore — Linux 原生分支（`linux-native`）

多解码器统一封装库的**纯 Linux**实现：pthread 解码线程 + POSIX 文件/环形缓冲 + ALSA 输出。
本分支不再包含 ESP-IDF、FreeRTOS、I2S 或任何“把 ESP API 假装成 Linux”的兼容封装层。

> `master` 仍然是 ESP-IDF 组件（ESP32-S3 验证），两份代码的公开 API 不同。
> 本分支定位为 ARMv7 Linux 与桌面 Linux；不要把它合回 `master`。
>
> 从旧 API / 兼容层迁移：见 [`docs/MIGRATION.md`](docs/MIGRATION.md)。

---

## 1. 与 `master` 的区别

| 项目 | `master`（ESP-IDF） | 本分支（`linux-native`） |
|---|---|---|
| 构建系统 | ESP-IDF component（`idf_component_register` + Kconfig） | 原生 CMake（`add_library` + options） |
| 输出 | 应用传入 `i2s_chan_handle_t` | 库内部打开 ALSA PCM（`cfg.alsa_device`） |
| 返回码 | `esp_err_t` / `ESP_ERR_*` | `int`，0 或负 errno（`-EINVAL`…） |
| 线程 | FreeRTOS `xTaskCreatePinnedToCore` | `pthread_create` + `pthread_join` |
| 同步 | `xSemaphore` / `volatile bool` | `pthread_mutex` / `std::atomic` / 条件变量 |
| 环形缓冲 | FreeRTOS `RingbufHandle_t` | 自带的 `espaudiocore_ringbuf_t`（线程安全） |
| 日志 | `ESP_LOGx` | `fprintf(stderr)` + 级别过滤（`ESPAUDIOCORE_LOG_LEVEL`） |
| 内存 | `heap_caps_malloc`（PSRAM/DMA 能力） | `posix_memalign` / `malloc` |
| I2S | `driver/i2s_std.h` | 不再存在；ALSA 通过 `dlopen("libasound.so.2")` 调用 |
| 路径 | 必须含挂载点的绝对路径 | 相对/绝对路径都可以 |

旧兼容层的 ESP 头（`esp_err.h`、`esp_log.h`、`esp_heap_caps.h`、`sdkconfig.h`、
`freertos/*`、`driver/i2s_std.h`、`linux_port.cpp`）**在本分支全部不需要**，也没有对应文件。

---

## 2. 目录

```
espaudiocore/
├─ include/
│  ├─ espaudiocore.h              # 公开 C API（唯一对外头文件）
│  └─ private/                    # 内部 C++ 接口
├─ src/
│  ├─ audio_port.c                # Linux 平台层：日志 / 对齐内存 / 错误映射
│  ├─ audio_ringbuf.cpp           # 线程安全环形缓冲
│  ├─ audio_player.cpp            # pthread 解码线程 + 状态机
│  ├─ audio_sink_alsa.cpp         # ALSA 输出（dlopen，无需 ALSA 头文件）
│  ├─ audio_sink_ringbuf.cpp      # 环形缓冲输出
│  ├─ audio_source_fs.cpp         # POSIX 文件源（read/pread/lseek）
│  ├─ audio_source_ringbuf.cpp    # 环形缓冲输入（流式）
│  ├─ audio_decoder_route.cpp     # 扩展名优先 + 魔数回退
│  ├─ decoder_*.cpp               # 8 种格式解码器
│  └─ id3_parser.cpp / xing_parser.cpp
├─ lib/                           # 第三方解码库（vendored）
├─ compat/                        # 仅第三方库用的 pgmspace/Arduino 旧头
├─ examples/eac_play.c            # 命令行示例 / 冒烟测试
├─ tests/                         # 自测、冒烟、PCM 位精确对比、素材生成
└─ scripts/build.sh               # 主机 / 交叉构建（rsync 到 ~ 后编译）
```

---

## 3. 构建

### 3.1 依赖

- Linux / WSL，`cmake >= 3.16`、`gcc/g++ >= 8`（C++17）
- 运行期：`libasound.so.2`（由目标系统提供；构建期**不需要** ALSA 开发包）
- 交叉编译：`arm-none-linux-gnueabihf` 工具链

### 3.2 主机构建

```bash
./scripts/build.sh
# 产物：~/espaudiocore-linux/build-host/{eac_play,libespaudiocore.a}
```

脚本会把源码 rsync 到 `~/espaudiocore-linux/src` 再编译，避免在 9p/网络文件系统上直接编译。

手动构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

格式裁剪（默认全开）：

```bash
cmake -S . -B build -DESPAUDIOCORE_ENABLE_WAVPACK=OFF -DESPAUDIOCORE_ENABLE_OPUS=OFF
```

### 3.3 ARMv7 交叉编译

```bash
./scripts/build.sh --cross
# 产物：~/espaudiocore-linux/build-arm/eac_play
```

工具链默认值可用环境变量覆盖：

```bash
TOOLCHAIN_DIR=/opt/gcc-arm ./scripts/build.sh --cross
```

---

## 4. 快速开始

```c
#include "espaudiocore.h"

static void on_event(void *user, espaudiocore_event_t ev, const char *msg) {
    (void)user;
    printf("event %d: %s\n", (int)ev, msg);
}

int main(void) {
    espaudiocore_cfg_t cfg = {0};
    cfg.alsa_device = "plughw:0,0";   /* NULL => "default" */
    cfg.on_event    = on_event;
    cfg.volume_db   = -6.0f;

    int dec_err = 0;
    int rc = espaudiocore_begin("/music/a.flac", &cfg, &dec_err);
    if (rc != 0) {
        fprintf(stderr, "begin: %s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
        return 1;
    }

    while (espaudiocore_is_running())
        usleep(100000);

    espaudiocore_stop();
    return 0;
}
```

编译：

```bash
gcc app.c -I/path/to/espaudiocore/include -L... -lespaudiocore -lpthread -ldl -lm
```

命令行工具：

```bash
eac_play song.mp3                                   # ALSA "default"
eac_play song.flac --device plughw:0,0              # 板载扬声器
eac_play song.flac --device plughw:0,0 --seek 60000 # 从 60 s 开始
eac_play song.ogg --raw-out /tmp/out.pcm --bits 16  # 解码到环形缓冲
```

板端 ALSA 需要 root 或 `audio` 组权限（`/dev/snd` 通常 660 root:audio）：

```bash
sudo -n ./eac_play /tmp/t.wav --device plughw:0,0
```

---

## 5. 线程与生命周期

- `espaudiocore_begin*()` 立即返回，解码在内部 pthread 中运行。
- 回调（event/meta/pcm）在解码线程上同步执行，**不要阻塞**，也不要调用
  `espaudiocore_stop()`（会检测到自 join 并返回 `ESPAUDIOCORE_ERR_STATE`）。
- `espaudiocore_stop()` 置停止标志并 `pthread_join`，然后关闭 ALSA（EOS 时先 drain，
  手动 stop 则丢弃未播数据）。所有阻塞点（ALSA 写、环形缓冲读写、文件读）都以
  100 ms 为片检查停止标志，因此 stop 有界。
- `pause()` 冻结位置；`resume()` 从冻结位置继续。
- 同一进程同时只允许一个会话；重复 `begin` 会先停掉旧会话。
- 应用层的 `espaudiocore_ringbuf_t` 必须在 `stop()` 之后再销毁。

---

## 6. 输出模式

| API | 输入 | 输出 | 用途 |
|---|---|---|---|
| `espaudiocore_begin()` | 文件 | ALSA | 常规播放 |
| `espaudiocore_begin_rb()` | 文件 | ringbuf | 自己处理 PCM（可视化/转码/下游） |
| `espaudiocore_begin_stream()` | ringbuf（压缩流） | ALSA | 网络流 / 上游解码器 |
| `espaudiocore_begin_stream_rb()` | ringbuf | ringbuf | 纯软件管道 |

ringbuf 源是纯顺序流：**不支持 seek**，必须显式给出格式；`get_duration_ms()`
可能返回 `<0`。ringbuf 输出不碰硬件，采样率变化通过 `get_format()` 报告，应用自行处理。

`output_bits`：

- `0`：跟随解码器位深；
- `16/24/32`：固定输出位深。16→24/32 左移补齐，24/32→16 右移截断；
- ALSA 格式：16→`S16_LE`，24→`S24_3LE`，32→`S32_LE`；
- 音量在 int32 域施加，0 dB 时走直通分支。

---

## 7. 错误码

成功返回 `0`，失败返回负 errno：

| 常量 | 值 | 含义 |
|---|---|---|
| `ESPAUDIOCORE_ERR_INVALID_ARG` | `-EINVAL` | 参数错误 |
| `ESPAUDIOCORE_ERR_NO_MEM` | `-ENOMEM` | 内存不足 |
| `ESPAUDIOCORE_ERR_NOT_FOUND` | `-ENOENT` | 文件/设备打不开 |
| `ESPAUDIOCORE_ERR_NOT_SUPPORTED` | `-ENOTSUP` | 无解码器 / 能力不支持 |
| `ESPAUDIOCORE_ERR_IO` | `-EIO` | 读写或 ALSA 失败 |
| `ESPAUDIOCORE_ERR_HEADER` | `-EBADMSG` | 头部/容器解析失败 |
| `ESPAUDIOCORE_ERR_STATE` | `-EPERM` | 当前状态不允许 |
| `ESPAUDIOCORE_ERR_TIMEOUT` | `-ETIMEDOUT` | 超时 |
| `ESPAUDIOCORE_ERR_DECODER` | `-EILSEQ` | 解码器报数据损坏 |

`dec_err` 出参仍然是**解码器原生错误码**（`AUDIO_DEC_ERR_*`），用于区分“格式不支持”
和“文件损坏”。

日志：`ESPAUDIOCORE_LOG_LEVEL=0..4`（NONE/ERROR/WARN/INFO/DEBUG），默认 3；
运行期 `espaudiocore_set_log_level()`。

---

## 8. 验证

所有测试脚本都在 `tests/`，测试素材用 ffmpeg 生成（`tests/gen_assets.sh`）。

| 测试 | 结果 |
|---|---|
| 主机 + ALSA `null`：WAV/MP3/AAC/M4A/FLAC/OGG/Opus/WavPack | 全部 STARTED→EOS→STOPPED |
| ARMv7 + ALSA `null`：8 种格式 | 全部通过 |
| ARMv7 + `plughw:0,0`：WAV / Opus(48k) / M4A / 24-bit 输出 | 打开、配置、按实时速率推进、正常停止 |
| ringbuf 输出 vs ffmpeg：WAV 16-bit、FLAC 16-bit、24-bit | **逐字节一致** |
| 真实音乐文件：FLAC 16/24-bit vs `flac -d` | **逐字节一致** |
| 真实音乐文件：MP3/AAC/Opus vs ffmpeg | 长度一致（MP3 少 1 帧）、SNR 54–88 dB |
| `eac_selftest`：pause 冻结 / resume / seek 1500 ms / EOS / stop | 通过 |

详细数据（真实文件、性能、seek 矩阵、sanitizer）见
[`docs/VALIDATION.md`](docs/VALIDATION.md)。

```bash
./scripts/build.sh
./tests/smoke.sh ~/espaudiocore-linux/build-host/eac_play
./tests/compare_pcm.sh ~/espaudiocore-linux/build-host/eac_play tests/assets
# 有损格式对比：pcm_compare <ours.pcm> <ref.pcm> <channels> <rate>
./scripts/build.sh --cross
./scripts/board_test.sh          # 板端 null
./scripts/board_test.sh --alsa   # 板端真实声卡（需 root 运行 eac_play）
```

---

## 9. 已知差异 / 限制

- 本分支只支持 Linux；ESP32 仍用 `master`。
- 输出只有 ALSA；没有 PulseAudio/PipeWire 专用后端（可通过 ALSA `default` 走插件）。
- 音量是软件音量（int32 域乘法），不控制声卡硬件音量。
- `pause()` 对不支持 `snd_pcm_pause` 的 PCM 插件退化为“停止喂数据”，
  ALSA 缓冲里的余音会播完。
- Opus/Vorbis 的 ringbuf **输入**不支持（容器需要 seek）；Opus/Vorbis 文件输入正常。
- 无 Xing/Info 头的 MP3：`duration=-1`，`seek_ms()` 失败；ADTS AAC 同样不支持 seek。
- 被截断的 FLAC 会按头部 STREAMINFO 报告时长，实际播放到数据结尾即 EOS。
- 板端 `/dev/snd` 权限不足时 `snd_pcm_open` 返回 `ENOENT`/`EACCES`，用 root 或加 `audio` 组。
- 第三方库有少量为 Linux 做的补丁，见下节。

---

## 10. 第三方库改动（相对上游）

| 文件 | 改动 | 原因 |
|---|---|---|
| `lib/libogg/ogg/os_types.h` | 内存宏改回 `malloc/calloc/realloc/free` | master 把 `_ogg_*` 重定向到 `heap_caps_*`（PSRAM），Linux 没有该 API |
| `lib/libflac/config.h` | 关闭 `FLAC__USE_ESP32_HEAP_CAPS_ALLOC` | 同上 |
| `lib/libhelix-mp3/mp3dec.h`、`assembly.h` | 平台分支增加 `__x86_64__`/`__aarch64__`/`__arm__` | 原文件只有 i386/thumb/ESP 分支，Linux 主机和 ARMv7 会 `#error` |
| `lib/libhelix-aac/aacdec.h`、`assembly.h` | 增加 `__aarch64__`/`__arm__`/`__x86_64__` | 同上 |
| `compat/pgmspace.h`、`compat/Arduino.h` | 新增（仅第三方库包含） | helix/ogg/opus 仍然 `#include <pgmspace.h>`/`<Arduino.h>`；这是 AVR 旧头，不是 OS 兼容层 |

除上述文件外，`lib/` 保持原样。

---

## 11. 许可

MIT（自有代码）。第三方解码库许可见 `THIRD_PARTY_LICENSES`。
