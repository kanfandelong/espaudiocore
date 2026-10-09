# espaudiocore API 参考

本文件是公共头 `<espaudiocore.h>` 的**精确参考**，逐项对应实现，不含开发过程与内部细节。
面向使用者的整体介绍见 [README.md](README.md)。

- 组件版本：**1.0.0**
- 目标框架：**ESP-IDF v6.1+**
- 语言：C（头文件带 `extern "C"` 保护，可直接从 C++ 调用）

---

## 1. 头文件

```c
#include "espaudiocore.h"
```

引入的内容：

| 符号 | 来源 | 条件 |
| --- | --- | --- |
| `i2s_chan_handle_t` | `driver/i2s_std.h` | 启用任一解码器时自动引入 |
| `RingbufHandle_t` | `freertos/ringbuf.h` | 始终 |
| `esp_err_t` | `esp_err.h` | 始终 |

---

## 2. 类型

### 2.1 `espaudiocore_err_t` — 返回码

返回码**复用 IDF 语义**，所以 `esp_err_to_name()` 能直接打印可读文本。

| 枚举 | 实际值 | 含义 |
| --- | --- | --- |
| `ESPAUDIOCORE_ERR_UNSUPPORTED` | `ESP_ERR_NOT_SUPPORTED` | 没有能处理的路由 |
| `ESPAUDIOCORE_ERR_OPEN_FAILED` | `ESP_ERR_NOT_FOUND` | 文件 / 路径打不开 |
| `ESPAUDIOCORE_ERR_HEADER` | `ESP_ERR_INVALID_RESPONSE` | 头部 / 容器解析失败 |
| `ESPAUDIOCORE_ERR_NOT_RUNNING` | `ESP_ERR_INVALID_STATE` | 当前未在播放 |
| `ESPAUDIOCORE_ERR_DECODER` | `ESP_FAIL` | 解码失败，详情看 `dec_err` 或 `EVT_ERROR` |

此外函数还会返回 IDF 通用码：`ESP_OK`、`ESP_ERR_INVALID_ARG`（参数为空、路径不以 `/` 开头）、
`ESP_ERR_NO_MEM`（内存不足）。

### 2.2 `espaudiocore_event_t` — 事件

| 枚举 | 触发时机 |
| --- | --- |
| `ESPAUDIOCORE_EVT_STARTED` | 会话建立成功、解码任务已启动，开始产出数据 |
| `ESPAUDIOCORE_EVT_PAUSED` | `pause()` 生效，已停止推送 |
| `ESPAUDIOCORE_EVT_RESUMED` | `resume()` 生效，恢复推送 |
| `ESPAUDIOCORE_EVT_SEEKED` | `seek_ms()` 完成，位置已生效 |
| `ESPAUDIOCORE_EVT_EOS` | 数据正常读到结尾 |
| `ESPAUDIOCORE_EVT_STOPPED` | 任务已退出；**EOS 之后也会自然补发一次** |
| `ESPAUDIOCORE_EVT_ERROR` | 出错；原因见 `msg` |

### 2.3 `espaudiocore_event_cb_t` — 事件回调

```c
typedef void (*espaudiocore_event_cb_t)(void *user, espaudiocore_event_t ev, const char *msg);
```

| 参数 | 说明 |
| --- | --- |
| `user` | 建立会话时 `cfg.user` 原样传入 |
| `ev` | 事件类型 |
| `msg` | 人类可读描述。**指向栈上缓冲，仅回调期间有效** |

**契约（必须遵守）**

1. 在**解码任务上下文**同步执行，**禁止阻塞**——不要在这里做 I2S 写、网络请求、GUI 重绘。
2. 需要异步处理时，在回调里 `xQueueSend()` 转交别的任务。
3. `msg` 要留存必须自行拷贝。
4. 回调可为 `NULL`。

### 2.4 `espaudiocore_meta_t` — 一条元数据

文本与图片共用同一结构。

```c
typedef struct {
    const char *type;      /* "Title"/"Artist"/"Album"/"APIC"/... */
    bool        is_binary; /* 图片等二进制为 true */
    const char *data;      /* 文本：NUL 结尾；二进制：原始字节 */
    size_t      len;       /* 文本：strlen；二进制：字节数（必需） */
    const char *mime;      /* 图片 MIME，如 "image/jpeg"；文本为 NULL */
    int         pic_type;  /* ID3 APIC 图片类型 0-20；非图片为 -1 */
} espaudiocore_meta_t;
```

> **零拷贝语义**：`data` 指向解析缓冲内部，本库不额外拷贝。所以一个带大封面的 MP3
> 不会让本库吃掉几百 KB 堆。**代价**是 `data` 仅在回调期间有效，需要留存必须自行拷贝；
> 封面这类数据建议"落盘 + 只留路径"。

### 2.5 `espaudiocore_meta_cb_t` — 元数据回调

```c
typedef void (*espaudiocore_meta_cb_t)(void *user, const espaudiocore_meta_t *meta);
```

与事件回调相同的上下文与禁阻塞约束。

### 2.6 `espaudiocore_format_t` — 解码格式

```c
typedef enum {
    ESPAUDIOCORE_FMT_UNKNOWN = 0,
    ESPAUDIOCORE_FMT_WAV,
    ESPAUDIOCORE_FMT_MP3,
    ESPAUDIOCORE_FMT_AAC,
    ESPAUDIOCORE_FMT_FLAC,
    ESPAUDIOCORE_FMT_VORBIS,
    ESPAUDIOCORE_FMT_OPUS,
    ESPAUDIOCORE_FMT_M4A,      /* M4A/MP4 容器中的 AAC-LC */
} espaudiocore_format_t;
```

**何时需要**：ringbuf 输入无法回退做魔数探测，必须由调用者显式给出。
文件输入时会按「扩展名 → 魔数」自动判定，无需传。M4A 当前要求可 seek 的文件输入。

### 2.7 `espaudiocore_cfg_t` — 会话配置

```c
typedef struct {
    uint32_t task_stack;    /* 0 = 用解码器推荐值（默认 8192，Opus 16384） */
    int      task_priority; /* 0 = 用 Kconfig 默认值 */
    int      task_core;     /* 默认 1；-1 = 不绑核 */
    uint32_t io_timeout_ms; /* 输出阻塞超时；0 = 无限等待（推荐） */
    uint8_t  output_bits;   /* 0 = 跟随 PCM；16/24/32 = 固定位宽 */
    float    volume_db;     /* 初始音量（dB），0 = 原始幅度 */
    espaudiocore_event_cb_t on_event; /* 可为 NULL */
    espaudiocore_meta_cb_t  on_meta;  /* 可为 NULL */
    void                   *user;     /* 回调 user 指针 */
} espaudiocore_cfg_t;
```

| 字段 | 说明 |
| --- | --- |
| `task_stack` | `0` 表示使用默认值 |
| `task_priority` | `0` 表示用 `CONFIG_ESPAUDIOCORE_TASK_PRIORITY` |
| `task_core` | 字面意思 |
| `io_timeout_ms` | `0` = 无限等待。阻塞点均可被 `stop()` 唤醒，推荐保持 `0` |
| `output_bits` | **两种模式**：`0` 跟随 PCM 位宽、不做处理；`16/24/32` 固定位宽，源位宽更窄则左移补齐、更宽则右移截断。例：PCM5102 这类固定 32-bit 槽宽的 DAC 填 `32`，16-bit 源会被左移 16 位 |
| `volume_db` | 见 [`espaudiocore_set_volume_db()`](#36-espaudiocore_set_volume_db) |

整个结构可零初始化（`= {}`），未设字段取默认值。传 `NULL` 等同于全默认。

---

## 3. 函数

### 3.1 会话建立

四种组合，**按输入 × 输出选择对应函数**：

| 输入 | 输出 | 函数 |
| --- | --- | --- |
| 文件 | I2S | [`espaudiocore_begin()`](#311-espaudiocore_begin) |
| 文件 | ringbuf | [`espaudiocore_begin_rb()`](#312-espaudiocore_begin_rb) |
| ringbuf | I2S | [`espaudiocore_begin_stream()`](#313-espaudiocore_begin_stream) |
| ringbuf | ringbuf | [`espaudiocore_begin_stream_rb()`](#314-espaudiocore_begin_stream_rb) |

四者都会**立即返回**（内部起解码任务），不等播放结束。

---

#### 3.1.1 `espaudiocore_begin()`

```c
esp_err_t espaudiocore_begin(const char *path, i2s_chan_handle_t tx,
                             const espaudiocore_cfg_t *cfg, int *dec_err);
```

打开本地文件并立即开始播放，输出到 I2S。

| 参数 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | **含挂载点的绝对路径**，如 `"/sdcard/a.mp3"`。不接受相对路径；不以 `/` 开头返回 `ESP_ERR_INVALID_ARG` |
| `tx` | in | 已 `init_std_mode` + `enable` 的 I2S TX 通道。**所有权归应用**，本库只写数据，并在采样率变化时重配其时钟 |
| `cfg` | in | 可为 `NULL` |
| `dec_err` | out | 解码器原生错误码，仅解码阶段失败时有效；可传 `NULL` |

返回：`ESP_OK`、`ESP_ERR_INVALID_ARG`、`ESP_ERR_NOT_FOUND`、`ESP_ERR_INVALID_RESPONSE`、
`ESP_ERR_NOT_SUPPORTED`、`ESP_ERR_NO_MEM`、`ESP_ERR_INVALID_STATE`。

---

#### 3.1.2 `espaudiocore_begin_rb()`

```c
esp_err_t espaudiocore_begin_rb(const char *path, RingbufHandle_t out,
                                const espaudiocore_cfg_t *cfg, int *dec_err);
```

与 `begin()` 的唯一区别是输出端：把解码后的 **PCM 写进 ringbuf**，由应用自行取走使用。

| 参数 | 说明 |
| --- | --- |
| `path` | 同 `begin()` |
| `out` | 输出 PCM 的 ringbuf，**所有权归调用者** |

> **采样率不由此函数处理**：输出为 ringbuf 时本库不控制硬件，
> 采样率通过 `espaudiocore_get_format()` 上报，由应用自行处理。

---

#### 3.1.3 `espaudiocore_begin_stream()`

```c
esp_err_t espaudiocore_begin_stream(RingbufHandle_t in, espaudiocore_format_t fmt,
                                    i2s_chan_handle_t tx, const espaudiocore_cfg_t *cfg,
                                    int *dec_err);
```

从 ringbuf 读**压缩数据流**并解码输出到 I2S（网络流、上游解码器等）。

| 参数 | 说明 |
| --- | --- |
| `in` | 输入 ringbuf（压缩/编码数据），所有权归调用者 |
| `fmt` | **必须显式指定数据格式** |

**该模式下的能力限制**

- `espaudiocore_seek_ms()` 返回 `ESP_ERR_NOT_SUPPORTED`
- 依赖 seek 的时长估算不可用，`espaudiocore_get_duration_ms()` 可能返回 `< 0`

---

#### 3.1.4 `espaudiocore_begin_stream_rb()`

```c
esp_err_t espaudiocore_begin_stream_rb(RingbufHandle_t in, espaudiocore_format_t fmt,
                                       RingbufHandle_t out, const espaudiocore_cfg_t *cfg,
                                       int *dec_err);
```

**两端都是 ringbuf** 的流式会话。适合完整流式管道：

```
任务A ──(压缩流)──> in ringbuf ──> 本库解码 ──> out ringbuf ──(PCM流)──> 任务B
```

采样率同样通过 `espaudiocore_get_format()` 上报，由应用处理。

---

### 3.2 播放控制

#### 3.2.1 `espaudiocore_stop()`

```c
esp_err_t espaudiocore_stop(void);
```

停止播放并释放资源，**阻塞**至解码任务真正退出。

所有阻塞点都可被本调用唤醒，正常是毫秒级返回。**返回后即可安全销毁 I2S 通道或 ringbuf。**

返回 `ESP_ERR_INVALID_STATE` 表示当前没有会话。

#### 3.2.2 `espaudiocore_pause()`

```c
esp_err_t espaudiocore_pause(void);
```

停止向输出推送新数据。

> 输出为 I2S 时，DMA 队列内的残留数据会继续播完，因此有一段
> `dma_desc_num × dma_frame_num ÷ sample_rate` 的余音，时长由应用创建 I2S 时的
> DMA 配置决定。需要更快响应就减小队列深度。

#### 3.2.3 `espaudiocore_resume()`

```c
esp_err_t espaudiocore_resume(void);
```

恢复播放。

#### 3.2.4 `espaudiocore_seek_ms()`

```c
esp_err_t espaudiocore_seek_ms(int64_t ms);
```

跳转到指定位置，**阻塞**至跳转真正生效。

| 返回 | 场景 |
| --- | --- |
| `ESP_OK` | 成功 |
| `ESP_ERR_NOT_SUPPORTED` | ringbuf 输入（纯顺序流，无 seek） |
| `ESP_ERR_INVALID_STATE` | 当前未在播放 |

> 跳转在**解码任务内串行执行**（seek 源 → 复位解码器 → 继续解码）。

---

### 3.3 音量

#### 3.3.1 `espaudiocore_set_volume_db()`

```c
esp_err_t espaudiocore_set_volume_db(float db);
```

设置输出音量。

| 参数 | 说明 |
| --- | --- |
| `db` | 增益，单位 dB。建议 `-60 .. +6`；`-60` 及以下视为静音 |

**实现语义**

- 在输出汇的写入路径上做整数缩放，**在 int32 域完成并带饱和处理**
- **I2S 与 ringbuf 两种输出语义一致**
- 增益为 `0 dB` 时不做任何乘加运算，直接写入（零额外开销）
- 正值放大，超过满量程会饱和削顶；负值衰减
- 可随时调用，立即生效，无需重开会话

> 若下游需要**未缩放**的原始 PCM，把音量保持 `0 dB` 即可。

#### 3.3.2 `espaudiocore_get_volume_db()`

```c
float espaudiocore_get_volume_db(void);
```

读取当前音量设置。

---

### 3.4 查询

#### 3.4.1 `espaudiocore_get_position_ms()`

```c
int64_t espaudiocore_get_position_ms(void);
```

当前播放位置（毫秒）。**已扣除 I2S DMA 队列内的残留时长**。
暂停期间位置冻结；未在播放时返回 `0`。

#### 3.4.2 `espaudiocore_get_duration_ms()`

```c
int64_t espaudiocore_get_duration_ms(void);
```

总时长（毫秒）。**`< 0` 表示未知**——流式源、无 Xing/VBR 头的 MP3 等。

> 部分格式（如无头时长信息的 AAC）只能在解码过程中按码率估算，
> 估算值会在可用后通过 `on_meta` 的 `"tlen"` 项上报，同时同步给本接口。

#### 3.4.3 `espaudiocore_get_format()`

```c
esp_err_t espaudiocore_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);
```

当前解码输出的格式。任一 `out` 参数可为 `NULL`。

用途取决于输出方式：

| 输出 | 用途 |
| --- | --- |
| I2S | 本库内部自动处理采样率变化；此接口用于观察 / UI 显示 |
| ringbuf | 本库不碰硬件，**这是应用层唯一的采样率信息源**，需自行处理（重采样 / 下游协商 / 忽略） |

#### 3.4.4 `espaudiocore_is_running()`

```c
bool espaudiocore_is_running(void);
```

是否正在播放（**含暂停态**）。

---

## 4. 所有权与生命周期

| 对象 | 所有权 | 说明 |
| --- | --- | --- |
| `i2s_chan_handle_t` | 应用 | 本库只写数据、按需重配时钟；不创建、不销毁 |
| 输入 `RingbufHandle_t` | 应用 | 本库只读 |
| 输出 `RingbufHandle_t` | 应用 | 本库只写 |
| 回调 `msg` / `meta->data` | 库 | **仅回调期间有效**，留存需拷贝 |
| `dec_err` 出参 | 应用 | |

**销毁顺序**：先 `espaudiocore_stop()`，再销毁 I2S 通道或 ringbuf。
`stop()` 返回即保证解码任务已退出，不会再触碰这些资源。

---

## 5. 线程模型

- 所有回调都在**解码任务**上下文执行，任务是本库内部创建的。
- 除回调外，公共 API 均可在任意任务调用。
- `stop()` / `seek_ms()` 会阻塞等待解码任务响应。
- 同一时刻只支持**一个**会话；重复 `begin*()` 前应先 `stop()`。

---

## 6. 最小示例

### 6.1 文件 → I2S

```c
#include "espaudiocore.h"

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    if (ev == ESPAUDIOCORE_EVT_EOS) {
        /* 播放结束。注意本回调运行在解码任务上，不要在这里阻塞 */
    }
}

void play_file(i2s_chan_handle_t tx)
{
    espaudiocore_cfg_t cfg = {
        .output_bits = 32,          /* PCM5102：固定 32-bit 槽宽 */
        .volume_db   = -6.0f,
        .on_event    = on_event,
    };
    int dec_err = 0;
    esp_err_t err = espaudiocore_begin("/sdcard/a.mp3", tx, &cfg, &dec_err);
    if (err != ESP_OK) {
        /* err 是 IDF 语义码，dec_err 是解码库原生码 */
        return;
    }
    /* 立即返回，播放由内部任务驱动 */
}
```

### 6.2 文件 → ringbuf（自己取 PCM）

```c
RingbufHandle_t out = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);

int dec_err = 0;
espaudiocore_cfg_t cfg = { .output_bits = 32, .volume_db = 0 };
ESP_ERROR_CHECK(espaudiocore_begin_rb("/sdcard/a.flac", out, &cfg, &dec_err));

/* 采样率由应用处理 */
uint32_t rate; uint8_t ch, bits;
if (espaudiocore_get_format(&rate, &ch, &bits) == ESP_OK) {
    my_i2s_reconfig(rate);
}

/* 另一个任务里从 out 取 PCM ... */
```

### 6.3 ringbuf → ringbuf（完整流式管道）

```c
RingbufHandle_t in  = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);
RingbufHandle_t out = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);

/* 任务A：把压缩数据写进 in（网络、文件、上游解码器……） */

int dec_err = 0;
ESP_ERROR_CHECK(espaudiocore_begin_stream_rb(in, ESPAUDIOCORE_FMT_MP3, out, NULL, &dec_err));

/* 任务B：从 out 取 PCM。注意 ringbuf 返回长度可能不是整帧，
   搬运到 I2S 前需按帧对齐，否则声道会错位。 */
```

---

## 7. 附：`on_meta` 中出现的 `type`

`type` 是**字符串标识**。文本类元数据**原样透传来源的键名，不做名称映射**——
所以不同格式看到的名字不一样（这正是可无损获取原始标签信息的代价）。

| `type` | `is_binary` | 出现于 | 含义 |
| --- | --- | --- | --- |
| ID3v2 帧 ID，如 `"TIT2"` `"TPE1"` `"TALB"` `"TRCK"` `"TYER"` `"TSSE"` | `false` | MP3 / AAC | 原始帧 ID，不做名称映射 |
| Vorbis comment 键名，如 `"title"` `"artist"` `"album"` `"encoder"` | `false` | FLAC / Vorbis / Opus | 原始键名（通常小写） |
| `"APIC"` | `true` | MP3 / AAC | ID3v2 封面（v2.2 的 `PIC` 也归一为 `APIC`） |
| `"PICTURE"` | `true` | FLAC | 封面块 |
| `"tlen"` | `false` | 全部 | 解码中估算出的总时长（毫秒，十进制字符串）；同一值也会同步给 `get_duration_ms()` |
| `"VENDOR"` | `false` | FLAC / Vorbis / Opus | Vorbis comment 的 vendor 字符串 |
| `"eof"` | `false` | MP3 / AAC | ID3 标签解析结束标记（`data` 为 `"id3"`），可用于判断标签区是否读完 |

二进制项（`is_binary == true`）的 `data` 是原始字节，长度见 `len`，类型见 `mime`，
`pic_type` 为图片类型（0–20）；文本项的 `mime` 为 `NULL`、`pic_type` 为 `-1`。

> 所有 `data` 都只在**回调期间有效**，需要留存必须自行拷贝。
