# espaudiocore

ESP-IDF 原生的**多解码器统一封装库**：一次调用即开始播放，解码在后台任务进行，由库内部管理解码任务。

- 目标框架：**ESP-IDF v6.1+**（验证硬件 ESP32-S3）
- 公共 API 参考：[**API.md**](API.md)
---

## 特性

- **6 种格式**：WAV / MP3 / AAC / FLAC / OGG Vorbis / Opus
- **格式自动判定**：文件输入按「扩展名 → 魔数」两级路由，支持 ID3v2 前缀跳过
- **两类输入**：本地文件（含挂载点的绝对路径）、ringbuf（网络流 / 等等）
- **两类输出**：I2S（库内部处理采样率变化）、ringbuf（应用自行取 PCM）
- **统一 int32 处理域**：所有解码器先把 PCM 归一化到 int32，音量缩放与位宽换算
  都在这一层完成，行为对两种输出一致
- **零拷贝元数据**：封面等大块数据不额外占堆，也不复制
- **标签与时长**：ID3v2、Vorbis comment、Xing / STREAMINFO
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

> **ringbuf 不能用于 Vorbis / Opus**：它们的封装库（vorbisfile / opusfile）要求可
> seek 的源，而 ringbuf 是纯顺序流。调用会返回 `ESP_ERR_NOT_SUPPORTED`，
> 不会静默失败。

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
        return;
    }
    /* 立即返回，播放与推流由库内部任务驱动，结束时会收到 EVT_EOS */
}
```

### 四个要点

1. **`begin()` 立即返回**，内部起解码任务；应用不需要轮询。
2. **路径必须是含挂载点的绝对路径**（`/sdcard/xxx`）。**本库不负责挂载**，挂载由应用完成。
3. **采样率**：输出为 I2S 时库内部自动重配置；输出为 ringbuf 时库应由应用层自行处理。
4. **错误码**：`esp_err_t` 返回值遵循 IDF 语义；
   解码库原生错误码走 `dec_err` 出参。

---

## 四种解码模式

按「输入 × 输出」选择函数：

| 输入 | 输出 | 函数 | 典型场景 |
| --- | --- | --- | --- |
| 文件 | I2S | `espaudiocore_begin()` | 本地播放器 |
| 文件 | ringbuf | `espaudiocore_begin_rb()` | 转码、自定义输出 |
| ringbuf | I2S | `espaudiocore_begin_stream()` | 网络电台、等等 |
| ringbuf | ringbuf | `espaudiocore_begin_stream_rb()` | 完整流式管道 |

完整的参数与返回值说明见 [API.md](API.md)。

### 流式管道示意

```
┌─────────┐  in ringbuf  ┌──────────────┐  out ringbuf  ┌─────────┐
│ 任务A    │ ───────────>│ espaudiocore │ ────────────> │ 任务B    │ ──> I2S
│ 压缩流   │             │   解码        │   PCM         │ 取 PCM  │
└─────────┘              └──────────────┘               └─────────┘
```

注意事项：**从 ringbuf 取到的长度不保证是整帧**（BYTEBUF 的环形布局会把数据
切在任意位置）。任务B 在写 I2S 前必须**按帧对齐**。

```c
/* 32-bit 立体声：帧 = 8 字节 */
const size_t frame = 8;
size_t aligned = got - (got % frame);
memcpy(tail, item + aligned, got - aligned);
```

---

## Kconfig 配置

`idf.py menuconfig → Component config → espaudiocore`

| 分组 | 选项 | 默认 | 说明 |
| --- | --- | --- | --- |
| — | `LOG_LEVEL` | 3 (INFO) | 运行时可被 `esp_log_level_set("espaudiocore", ...)` 覆盖 |
| **Decoders** | `ENABLE_WAV` / `MP3` / `AAC` / `FLAC` / `VORBIS` / `OPUS` | 全开 | 关闭可显著减小固件；Opus 内存占用较大，无 PSRAM 建议关 |
| **Output** | `OUTPUT_BITS` | 0 | `0` = 跟随 PCM；`16/24/32` = 固定位宽。**仅作为 `cfg.output_bits == 0` 时的默认值** |
| | `DEFAULT_VOLUME_DB` | 0 | 仅作为 `cfg.volume_db` 未设置时的默认值 |
| **Memory** | `FILE_SOURCE_MODE` | Self-managed window | 文件读取策略，见下 |
| | `SOURCE_BUFFER_SIZE` | 32768 | 自管理窗口大小 |
| | `VFS_BUFFER_SIZE` | 65536 | 纯 VFS 模式下的 `setvbuf` 缓冲 |
| | `PSRAM_REQUIRED` | n | 打开后 PSRAM 申请失败直接报错，不回退内部 RAM |
| **Task** | `TASK_STACK_DEFAULT` | 8192 | 兜底栈大小；各解码器可在路由表给推荐值（Opus 为 16384） |
| | `TASK_PRIORITY` | 8 | 解码任务优先级 |
| | `TASK_CORE` | 1 | 解码任务核心亲和 |
| — | `FLAC_XTENSA_ASM` | n | ESP32-S3 的 FLAC LPC 汇编，需同步在 `lib/libflac/config.h` 打开 `EN_ESP_ASM`；效果并不显著 (仅限xtensa内核) |

### 文件数据源的两种模式

| 模式 | 做法 | 特点 |
| --- | --- | --- |
| **Pure VFS + setvbuf**（默认） | 用 `setvbuf` 把 stdio 缓冲指到 PSRAM，不自管理窗口 | 路径更简单；`peek` 用 `fseeko`/`fread`/`fseeko` 实现，多两次 seek |
| **Self-managed window** | 自维护预读窗口，`setvbuf(_IONBF)` 关掉 stdio 缓冲避免双重缓冲 | `peek` 不产生额外 seek；窗口边界逻辑较复杂 |

两者都可把缓冲放进 PSRAM（优先 `SPIRAM|DMA`，再退 `SPIRAM`，最后内部 RAM）。
---

## 输出位宽与音量

这两项的行为对 **I2S 与 ringbuf 两种输出完全一致**。

### 位宽

`cfg.output_bits`（或 Kconfig 默认值）：

| 取值 | 行为 |
| --- | --- |
| `0` | 跟随 PCM 位宽，不做任何换算 |
| `16/24/32` | 固定位宽。源位宽更窄则**左移补齐**，更宽则**右移截断** |

例：PCM5102 这类固定 32-bit 槽宽的 DAC 填 `32`，16-bit 源会被左移 16 位，
24-bit FLAC 保持完整精度不被降到 16-bit。

### 音量

`cfg.volume_db` / `espaudiocore_set_volume_db()`：

- 在输出路径上做整数缩放，**在 int32 域完成并带饱和处理**
- `0 dB` 时**不做任何乘加**，直接写入（零额外开销）
- 正值放大，超过满量程会饱和削顶
- 可播放中随时调用

> 需要**未缩放**的原始 PCM 时，把音量保持 `0 dB`。

---

### 统一 int32 处理域

所有解码器的输出先归一化到 int32（16-bit 源 `<<16`、24-bit 源 `<<8`、32-bit 原值），
音量缩放与位宽换算都在这一层完成，再交给输出汇。好处：

- 位宽换算只有一处实现，两种输出不会出现行为分叉
- 高分辨率源在整个链路里不掉精度
- 增益为 0 dB 时可整体跳过，无额外开销

### 采样率归属（D-9）

| 输出 | 谁负责采样率 |
| --- | --- |
| I2S | **本库**：检测到变化时 `disable → reconfig_std_clock → enable` |
| ringbuf | **应用**：本库不碰硬件，只通过 `espaudiocore_get_format()` 上报 |

### 阻塞与停止

输出汇的写入采用「有界等待 + 每 100ms 检查停止标志」的有界循环，
所以 `stop()` 能在毫秒级唤醒所有阻塞点，不会卡在 DMA 或满 ringbuf 上。

---

## 已知限制

| 限制 | 说明 |
| --- | --- |
| **ringbuf 输入不支持 Vorbis / Opus** | 封装库要求可 seek，返回 `ESP_ERR_NOT_SUPPORTED` |
| **ringbuf 输入无法探测格式** | 必须显式传 `espaudiocore_format_t` |
| **M4A 未实现** | `ESPAUDIOCORE_FMT_M4A` 枚举已保留，容器解析未实现 |
| **同一时刻单会话** | 重复 `begin*()` 前应先 `stop()` |

---

## 目录结构

```
espaudiocore/
├── API.md                  公共 API 参考
├── README.md               本文件
├── CMakeLists.txt          组件构建（第三方库源文件清单与逐库 include 隔离）
├── Kconfig                 格式开关 / 输出 / 内存 / 任务参数
├── idf_component.yml       idf >= 6.1.0
├── THIRD_PARTY_LICENSES
├── include/
│   ├── espaudiocore.h      唯一公共头
│   └── private/            内部接口
├── src/
│   ├── espaudiocore.cpp        公共 API 薄封装
│   ├── audio_player.cpp        状态机 + 解码任务
│   ├── audio_decoder_route.cpp 路由表与匹配
│   ├── audio_source_fs.cpp     文件源
│   ├── audio_source_ringbuf.cpp ringbuf 源（纯顺序流，前瞻缓存实现 peek）
│   ├── audio_sink_i2s.cpp      I2S 输出
│   ├── audio_sink_ringbuf.cpp  ringbuf 输出（PCM）
│   ├── decoder_{wav,mp3,aac,flac,vorbis,opus}.cpp
│   ├── id3_parser.cpp / xing_parser.cpp
│   └── port/                   日志 / 内存 / 错误转换 + 第三方库 shim
└── lib/                    第三方解码库
```

---

## 为通过编译对第三方库所做的改动

第三方库原则上原样保留，但针对部分进行了些许修改。

---

## 许可

本组件自有代码：**MIT**。
第三方解码库的许可证见 [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)。
