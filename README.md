# espaudiocore

ESP-IDF 原生的**多解码器统一封装库**：一次调用即开始播放，解码在后台任务进行，由库内部管理解码任务。

- 目标框架：**ESP-IDF v6.1+**（验证硬件 ESP32-S3）
- 公共 API 参考：[**API.md**](API.md)

---

## 特性

- **7 种格式**：WAV / MP3 / AAC / FLAC / OGG Vorbis / Opus / WavPack
- **格式自动判定**：文件输入按「扩展名 → 魔数」两级路由，支持 ID3v2 前缀跳过
- **两类输入**：本地文件（含挂载点的绝对路径）、ringbuf（网络流 / 上游解码器）
- **两类输出**：I2S（库内部处理采样率变化）、ringbuf（应用自行取 PCM）
- **高分辨率保留**：24/32-bit 源不会被降成 16-bit
- **零拷贝元数据**：封面等大块数据不额外占堆，也不复制
- **可选 PSRAM**：大缓冲优先放 PSRAM，自动降级到内部 RAM

---

## 支持格式

| 格式 | 扩展名 | 解码库 | 文件输入 | ringbuf 输入 | 高位深 |
| --- | --- | --- | --- | --- | --- |
| WAV | `.wav` | 内置 | ✅ | ✅ | 8/16/24/32-bit、float32 |
| MP3 | `.mp3` | libhelix-mp3 | ✅ | ✅ | — |
| AAC | `.aac` | libhelix-aac（含 SBR） | ✅ | ✅ | — |
| FLAC | `.flac` | libFLAC | ✅ | ✅ | 16/24-bit |
| OGG Vorbis | `.ogg` | libvorbis + libogg | ✅ | ❌ | — |
| Opus | `.opus` | libopus + opusfile | ✅ | ❌ | — |
| WavPack | `.wv / .wvc` | libwavpack | ✅ | ✅ | 16/24、float32 |

> ringbuf 不能用于 Vorbis / Opus：其封装库要求可 seek 的源，调用会返回
> `ESP_ERR_NOT_SUPPORTED`，不会静默失败。

---

## 快速开始

```c
#include "espaudiocore.h"

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    if (ev == ESPAUDIOCORE_EVT_EOS) {
        /* 播放结束。此回调运行在解码任务上，禁止阻塞 */
    }
}

void app_main(void)
{
    /* I2S 通道由应用创建：GPIO / DMA / init_std_mode / enable 都在应用侧 */
    i2s_chan_handle_t tx = my_i2s_init();

    espaudiocore_cfg_t cfg = {
        .output_bits = 32,     /* PCM5102：固定 32-bit 槽宽 */
        .volume_db   = -6.0f,
        .on_event    = on_event,
    };

    int dec_err = 0;
    esp_err_t err = espaudiocore_begin("/sdcard/a.mp3", tx, &cfg, &dec_err);
    if (err != ESP_OK) {
        /* err 是 IDF 语义码；dec_err 是解码库原生码 */
        ESP_LOGE(TAG, "begin: %s (dec_err=%d)", esp_err_to_name(err), dec_err);
    }
}
```

要点：

- `begin()` **立即返回**，内部起解码任务，应用不需要轮询。
- 路径必须是**含挂载点的绝对路径**（`/sdcard/xxx`）。
- 输出为 ringbuf 时，采样率需应用自行处理（读 `espaudiocore_get_format()`）。
- 错误分两层：`esp_err_t` 遵循 IDF 语义；解码库原生码走 `dec_err` 出参。

---

## 四种解码模式

| 输入 | 输出 | 函数 | 典型场景 |
| --- | --- | --- | --- |
| 文件 | I2S | `espaudiocore_begin()` | 本地播放器 |
| 文件 | ringbuf | `espaudiocore_begin_rb()` | 转码、自定义输出 |
| ringbuf | I2S | `espaudiocore_begin_stream()` | 网络电台 |
| ringbuf | ringbuf | `espaudiocore_begin_stream_rb()` | 流式管道 |

参数与返回值说明见 [API.md](API.md)。

---

## 添加到工程

把 `espaudiocore/` 放进工程的 `components/` 目录即可，无外部依赖（解码库已随组件附带）。

```cmake
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES espaudiocore)
```

---

## 配置

`idf.py menuconfig → Component config → espaudiocore`

| 分组 | 选项 | 默认 | 说明 |
| --- | --- | --- | --- |
| — | `LOG_LEVEL` | 3 (INFO) | 运行时可被 `esp_log_level_set("espaudiocore", ...)` 覆盖 |
| **Decoders** | `ENABLE_WAV` / `MP3` / `AAC` / `FLAC` / `VORBIS` / `OPUS` | 全部启用 | 关闭可显著减小固件；Opus 内存占用较大，无 PSRAM 建议关 |
| **Output** | `OUTPUT_BITS` | 0 | `0` = 跟随 PCM；`16/24/32` = 固定位宽（`cfg.output_bits` 优先） |
| | `DEFAULT_VOLUME_DB` | 0 | 仅作为 `cfg.volume_db` 未设置时的默认值 |
| **Memory** | `FILE_SOURCE_MODE` | Pure VFS + setvbuf | 文件读取策略：纯 VFS 缓冲，或自维护预读窗口 |
| | `SOURCE_BUFFER_SIZE` | 32768 | 自管理窗口大小 |
| | `VFS_BUFFER_SIZE` | 65536 | 纯 VFS 模式下的 `setvbuf` 缓冲 |
| | `PSRAM_REQUIRED` | n | 打开后 PSRAM 申请失败直接报错，不回退内部 RAM |
| **Task** | `TASK_STACK_DEFAULT` | 8192 | 兜底栈大小；各解码器可在路由表给推荐值（Opus 为 16384） |
| | `TASK_PRIORITY` | 8 | 解码任务优先级 |
| | `TASK_CORE` | 1 | 解码任务核心亲和 |
| — | `FLAC_XTENSA_ASM` | n | ESP32-S3 的 FLAC LPC 汇编，需同步在 `lib/libflac/config.h` 打开 `EN_ESP_ASM`；效果并不显著（仅限 xtensa 内核） |

输出位宽与音量也可在运行时通过 `espaudiocore_cfg_t` 设置：

- `output_bits`：`0` 跟随 PCM；`16/24/32` 固定位宽，源更窄则左移补齐、更宽则右移截断。
- `volume_db`：整数缩放带饱和处理；`0 dB` 时不做任何乘加。

---

## 已知限制

| 限制 | 说明 |
| --- | --- |
| ringbuf 输入不支持 seek | 部分解码库可能无法在流式输入时正常工作 |
| ringbuf 输入无法探测格式 | 必须显式传入编码格式 `espaudiocore_format_t` |
| M4A 未实现 | `ESPAUDIOCORE_FMT_M4A` 枚举已保留，容器解析未实现 |
| 同一时刻单会话 | 重复 `begin*()` 前应先 `stop()` |

---

## 许可

组件自有代码：**MIT**。
第三方解码库原则上原样保留，仅做了必要的最小适配；许可证见 [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)。
