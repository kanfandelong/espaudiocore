# espaudiocore API 参考（Linux 原生分支）

对应头文件：`include/espaudiocore.h`（唯一对外头文件）。
平台：Linux / POSIX，输出后端为 ALSA（运行期通过 `dlopen` 加载 `libasound.so.2`）。

约定：

- 成功返回 `0`；失败返回**负 errno**（见第 2 节）。
- 同一进程同时只允许一个播放会话。
- 回调在解码线程上同步执行，禁止阻塞，禁止调用 `espaudiocore_stop()`。

---

## 1. 版本与日志

```c
#define ESPAUDIOCORE_VERSION_MAJOR 2
#define ESPAUDIOCORE_VERSION_MINOR 0
#define ESPAUDIOCORE_VERSION_PATCH 0
#define ESPAUDIOCORE_VERSION_STRING "2.0.0"

void espaudiocore_set_log_level(int level);
int  espaudiocore_get_log_level(void);
```

日志级别：

| 值 | 名称 | 说明 |
|---|---|---|
| 0 | `ESPAUDIOCORE_LOG_NONE` | 关闭 |
| 1 | `ESPAUDIOCORE_LOG_ERROR` | 错误 |
| 2 | `ESPAUDIOCORE_LOG_WARN` | 警告 |
| 3 | `ESPAUDIOCORE_LOG_INFO` | 默认 |
| 4 | `ESPAUDIOCORE_LOG_DEBUG` | 调试 |

启动时读取环境变量 `ESPAUDIOCORE_LOG_LEVEL` 作为初始值。日志写 stderr，
格式：`[秒.毫秒] I espaudiocore: ...`。

---

## 2. 错误码

```c
typedef enum {
    ESPAUDIOCORE_OK = 0,
    ESPAUDIOCORE_ERR_INVALID_ARG   = -EINVAL,
    ESPAUDIOCORE_ERR_NO_MEM        = -ENOMEM,
    ESPAUDIOCORE_ERR_NOT_FOUND     = -ENOENT,
    ESPAUDIOCORE_ERR_NOT_SUPPORTED = -ENOTSUP,
    ESPAUDIOCORE_ERR_IO            = -EIO,
    ESPAUDIOCORE_ERR_HEADER        = -EBADMSG,
    ESPAUDIOCORE_ERR_STATE         = -EPERM,
    ESPAUDIOCORE_ERR_TIMEOUT       = -ETIMEDOUT,
    ESPAUDIOCORE_ERR_DECODER       = -EILSEQ,
} espaudiocore_err_t;

const char *espaudiocore_strerror(int err);
```

`espaudiocore_strerror()` 对 0 返回 `"OK"`，对负值返回 `strerror(-err)`，永不返回 NULL。

---

## 3. 事件

```c
typedef enum {
    ESPAUDIOCORE_EVT_STARTED = 0,
    ESPAUDIOCORE_EVT_PAUSED,
    ESPAUDIOCORE_EVT_RESUMED,
    ESPAUDIOCORE_EVT_SEEKED,
    ESPAUDIOCORE_EVT_EOS,
    ESPAUDIOCORE_EVT_STOPPED,
    ESPAUDIOCORE_EVT_ERROR,
} espaudiocore_event_t;

typedef void (*espaudiocore_event_cb_t)(void *user, espaudiocore_event_t ev,
                                        const char *msg);
```

| 事件 | 时机 |
|---|---|
| `STARTED` | `begin()` 成功、解码线程已启动 |
| `PAUSED` / `RESUMED` | `pause()` / `resume()` 生效 |
| `SEEKED` | `seek_ms()` 在解码线程里完成 |
| `EOS` | 数据正常读到结尾（ALSA 已 drain） |
| `STOPPED` | 解码线程退出（EOS 之后也会发） |
| `ERROR` | 出错；`msg` 为原因 |

`msg` 指向栈上缓冲，仅在回调期间有效。回调在解码线程执行。

---

## 4. 元数据

```c
typedef struct {
    const char *type;      /* "Title"/"Artist"/"Album"/"APIC"/... */
    bool        is_binary;
    const char *data;      /* 文本 NUL 结尾；二进制原始字节 */
    size_t      len;
    const char *mime;      /* 图片 MIME；文本为 NULL */
    int         pic_type;  /* APIC picture type；非图片 -1 */
} espaudiocore_meta_t;

typedef void (*espaudiocore_meta_cb_t)(void *user,
                                       const espaudiocore_meta_t *meta);
```

APIC 的 `data` 指向解析缓冲，仅在回调期间有效；需要留存请在回调里复制或落盘。
MP3/AAC 在解码过程中估算出的时长也会以 `type == "tlen"` 的文本元数据上报。

各容器的标签来源：

| 容器 | 文本标签 | 封面 |
|---|---|---|
| MP3 / AAC / M4A | ID3v2 帧（`TIT2`/`TPE1`/…，`type` 为帧 ID） | ID3v2 `APIC`（`type == "APIC"`） |
| FLAC | `VORBIS_COMMENT` | `METADATA_BLOCK_PICTURE`（`type == "PICTURE"`） |
| Ogg Vorbis / Opus | `VORBIS_COMMENT` 键名 | `METADATA_BLOCK_PICTURE`、旧式 `COVERART`/`COVERARTMIME`（`type == "PICTURE"`） |
| WAV | `LIST/INFO` 的 `INAM`/`IART`/`IPRD` → `Title`/`Artist`/`Album`；`id3 ` chunk 的 ID3v2 帧 | `id3 ` chunk 里的 `APIC` |

WAV 的 `id3 ` chunk 可以出现在 `data` 之前或之后；可 seek 的源会继续扫描音频之后
的 chunk，ring buffer 等纯流式源只解析 `data` 之前的部分。ID3v2 标签解析结束会
上报一条 `type == "eof"` 的标记。

---

## 5. PCM 旁路

```c
typedef struct {
    const int32_t *samples;  /* 交错：samples[frame * channels + ch] */
    size_t         frames;
    uint32_t       rate;
    uint8_t        channels; /* 1/2 */
    uint8_t        bits;     /* 16/24/32 */
} espaudiocore_pcm_t;

typedef void (*espaudiocore_pcm_cb_t)(void *user,
                                      const espaudiocore_pcm_t *pcm);
```

数值为左对齐 int32：

- `bits == 16`：`value = int16 << 16`；
- `bits == 24`：`value = int24 << 8`；
- `bits == 32`：原值。

回调在**音量缩放之前**、ALSA 写入之前触发，适合 FFT / 波形。仅 ALSA 输出有效；
ringbuf 输出时应用已经拥有 PCM，不再触发。数据零拷贝，仅在回调期间有效。

---

## 6. 格式枚举

```c
typedef enum {
    ESPAUDIOCORE_FMT_UNKNOWN = 0,
    ESPAUDIOCORE_FMT_WAV,
    ESPAUDIOCORE_FMT_MP3,
    ESPAUDIOCORE_FMT_AAC,
    ESPAUDIOCORE_FMT_FLAC,
    ESPAUDIOCORE_FMT_VORBIS,
    ESPAUDIOCORE_FMT_OPUS,
    ESPAUDIOCORE_FMT_M4A,
    ESPAUDIOCORE_FMT_WAVPACK,
} espaudiocore_format_t;
```

枚举始终完整；编译期裁剪掉的解码器会在运行时返回
`ESPAUDIOCORE_ERR_NOT_SUPPORTED`。

---

## 7. 配置

```c
typedef struct {
    const char *alsa_device;
    uint32_t    alsa_latency_ms;
    uint32_t    io_timeout_ms;
    uint8_t     output_bits;
    float       volume_db;

    espaudiocore_event_cb_t on_event;
    espaudiocore_meta_cb_t  on_meta;
    espaudiocore_pcm_cb_t   on_pcm;
    void                   *user;
} espaudiocore_cfg_t;
```

| 字段 | 默认值 | 说明 |
|---|---|---|
| `alsa_device` | NULL → `"default"` | ALSA PCM 名；板端常用 `"plughw:0,0"` |
| `alsa_latency_ms` | 0 → 200 ms | 目标延迟，钳位到 20..2000 |
| `io_timeout_ms` | 0 | 输出写超时；0 = 不超时 |
| `output_bits` | 0 | 0 = 跟随源；16/24/32 = 固定输出位深 |
| `volume_db` | 0.0 | 初始音量；≤ -60 视为静音 |
| `on_event` | NULL | 事件回调 |
| `on_meta` | NULL | 元数据回调 |
| `on_pcm` | NULL | PCM 旁路（仅 ALSA 输出） |
| `user` | NULL | 回调上下文 |

配置结构建议零初始化（`espaudiocore_cfg_t cfg = {0};`）后按需赋值。

ALSA 在 `begin()` 时打开（错误立即暴露），`hw_params` 在解码器报出真实
采样率/位深/声道后、第一次写入前应用。采样率中途变化时自动 `drop` + 重配。

---

## 8. 环形缓冲

```c
typedef struct espaudiocore_ringbuf espaudiocore_ringbuf_t;

espaudiocore_ringbuf_t *espaudiocore_ringbuf_create(size_t capacity);
void espaudiocore_ringbuf_destroy(espaudiocore_ringbuf_t *rb);

size_t espaudiocore_ringbuf_write(espaudiocore_ringbuf_t *rb,
                                  const void *data, size_t len, int timeout_ms);
size_t espaudiocore_ringbuf_read(espaudiocore_ringbuf_t *rb,
                                 void *data, size_t len, int timeout_ms);

size_t espaudiocore_ringbuf_used(const espaudiocore_ringbuf_t *rb);
size_t espaudiocore_ringbuf_space(const espaudiocore_ringbuf_t *rb);
size_t espaudiocore_ringbuf_capacity(const espaudiocore_ringbuf_t *rb);
void   espaudiocore_ringbuf_clear(espaudiocore_ringbuf_t *rb);
```

- 线程安全（内部 mutex + condvar）。
- `timeout_ms`：`<0` 无限等待，`0` 非阻塞，`>0` 最多等这么多毫秒。
- `write` 可能只写入一部分（超时/停止），返回实际写入字节数。
- `read` 有数据就返回（最多 `len`），不保证填满。
- 会话停止时，内部读写会在 100 ms 内被停止标志打断。
- `destroy` 必须在 `espaudiocore_stop()` 之后调用。

示例：

```c
espaudiocore_ringbuf_t *rb = espaudiocore_ringbuf_create(64 * 1024);
espaudiocore_begin_rb("a.flac", rb, NULL, &dec_err);

uint8_t buf[4096];
while (espaudiocore_is_running() || espaudiocore_ringbuf_used(rb) > 0) {
    size_t n = espaudiocore_ringbuf_read(rb, buf, sizeof(buf), 100);
    if (n) fwrite(buf, 1, n, stdout);
}
espaudiocore_stop();
espaudiocore_ringbuf_destroy(rb);
```

---

## 9. 生命周期

### `espaudiocore_begin`

```c
int espaudiocore_begin(const char *path,
                       const espaudiocore_cfg_t *cfg, int *dec_err);
```

打开本地文件并通过 ALSA 播放。

| 参数 | 方向 | 说明 |
|---|---|---|
| `path` | in | 文件路径（相对/绝对均可） |
| `cfg` | in | 可为 NULL（默认配置） |
| `dec_err` | out | 解码器原生错误码；可 NULL |

返回：0，或 `INVALID_ARG` / `NOT_FOUND` / `HEADER` / `NOT_SUPPORTED` /
`NO_MEM` / `IO`（ALSA 打不开或配置失败）。

### `espaudiocore_begin_rb`

```c
int espaudiocore_begin_rb(const char *path, espaudiocore_ringbuf_t *out,
                          const espaudiocore_cfg_t *cfg, int *dec_err);
```

文件 → ringbuf。不碰 ALSA，`cfg->alsa_device` 忽略。`out` 生命周期由应用管理。

### `espaudiocore_begin_stream`

```c
int espaudiocore_begin_stream(espaudiocore_ringbuf_t *in,
                              espaudiocore_format_t fmt,
                              const espaudiocore_cfg_t *cfg, int *dec_err);
```

ringbuf 中的**压缩数据** → ALSA。纯顺序流：不支持 seek、不做格式探测，
`fmt` 必须正确；`get_duration_ms()` 可能 `< 0`。

### `espaudiocore_begin_stream_rb`

```c
int espaudiocore_begin_stream_rb(espaudiocore_ringbuf_t *in,
                                 espaudiocore_format_t fmt,
                                 espaudiocore_ringbuf_t *out,
                                 const espaudiocore_cfg_t *cfg, int *dec_err);
```

压缩流 → PCM 流，纯软件管道。

### `espaudiocore_stop`

```c
int espaudiocore_stop(void);
```

置停止标志并 join 解码线程；EOS 时解码线程已 drain，手动 stop 则丢弃 ALSA
未播数据并关闭设备。返回后即可销毁应用层资源（ringbuf 等）。无会话时返回
`ESPAUDIOCORE_ERR_STATE`。
在解码线程回调里调用会返回 `ESPAUDIOCORE_ERR_STATE`（拒绝自 join）。

### `espaudiocore_pause` / `espaudiocore_resume`

```c
int espaudiocore_pause(void);
int espaudiocore_resume(void);
```

暂停位置被冻结，恢复后从冻结位置继续计时。ALSA 驱动支持 `snd_pcm_pause`
时直接暂停设备；否则退化为停止喂数据（缓冲余音会播完）。

### `espaudiocore_seek_ms`

```c
int espaudiocore_seek_ms(int64_t ms);
```

阻塞直到 seek 完成（最多约 200 ms 等待解码线程消费请求）。ALSA 输出会先丢弃
seek 前已排队的 PCM，避免听到旧位置的尾音；ringbuf 输出不会清空应用队列。

| 返回 | 条件 |
|---|---|
| 0 | 成功 |
| `ESPAUDIOCORE_ERR_NOT_SUPPORTED` | ringbuf 输入（无 seek） |
| `ESPAUDIOCORE_ERR_STATE` | 当前没有会话 |
| `ESPAUDIOCORE_ERR_INVALID_ARG` | `ms < 0` |
| `ESPAUDIOCORE_ERR_TIMEOUT` | 200 ms 内解码线程未消费请求（例如暂停中） |

### `espaudiocore_set_volume_db` / `get_volume_db`

```c
int   espaudiocore_set_volume_db(float db);
float espaudiocore_get_volume_db(void);
```

软件音量（Q16 定点，int32 域乘法，带饱和）。0 dB 为直通。ALSA 与 ringbuf
输出都生效。

---

## 10. 查询

```c
int64_t espaudiocore_get_position_ms(void);
int64_t espaudiocore_get_duration_ms(void);
int     espaudiocore_get_format(uint32_t *rate, uint8_t *channels,
                                uint8_t *bits);
bool    espaudiocore_is_running(void);
```

- `get_position_ms()`：已扣除 ALSA 缓冲中尚未播出的帧；暂停时冻结；无会话返回 0。
- `get_duration_ms()`：`<0` 表示未知（流式源、无 Xing 头等）。
- `get_format()`：任一 out 参数可为 NULL；无会话返回 `ESPAUDIOCORE_ERR_STATE`。
  ALSA 输出时用于显示/诊断；ringbuf 输出时是应用**唯一**的格式信息来源。
- `is_running()`：会话存在且解码线程尚未结束（EOS 后为 false）。

---

## 11. 完整示例（播放到 EOS）

```c
#include "espaudiocore.h"
#include <stdio.h>
#include <unistd.h>

static void on_event(void *u, espaudiocore_event_t ev, const char *msg) {
    (void)u;
    fprintf(stderr, "[ev] %d %s\n", (int)ev, msg);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;

    espaudiocore_cfg_t cfg = {0};
    cfg.alsa_device = "plughw:0,0";
    cfg.output_bits = 16;
    cfg.volume_db   = -6.0f;
    cfg.on_event    = on_event;

    int dec_err = 0;
    int rc = espaudiocore_begin(argv[1], &cfg, &dec_err);
    if (rc) {
        fprintf(stderr, "begin: %s (dec_err=%d)\n",
                espaudiocore_strerror(rc), dec_err);
        return 1;
    }

    while (espaudiocore_is_running())
        usleep(100000);

    printf("played %lld / %lld ms\n",
           (long long)espaudiocore_get_position_ms(),
           (long long)espaudiocore_get_duration_ms());
    espaudiocore_stop();
    return 0;
}
```

---

## 12. 线程与资源约定

| 对象 | 所有者 | 说明 |
|---|---|---|
| ALSA 设备 | 本库 | `begin()` 打开，`stop()` drain + close |
| 解码线程 | 本库 | `stop()` join；EOS 后线程自行退出，会话保留到 stop |
| 输入/输出 `espaudiocore_ringbuf_t` | 应用 | 必须先 `stop()` 再 `destroy` |
| 文件句柄 | 本库 | 源对象析构时关闭 |

- `begin/pause/resume/seek/stop` 建议由同一控制线程调用。
- `set_volume_db()` 可与其他线程并发（内部原子变量）。
- 事件/元数据/PCM 回调运行在解码线程，不能阻塞。
- 回调里可以读 `get_position_ms()` / `get_format()`（内部有快照锁）。
