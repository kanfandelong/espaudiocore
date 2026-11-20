# 从 ESP-IDF / 兼容层迁移到 `linux-native`

本文给两类调用者：

1. 原来用 `port/linux` 兼容层（`esp_err.h`、`freertos/*`、`driver/i2s_std.h`、
   `linux_i2s_open()`）在 Linux 上编译 `master` 的工程；
2. 直接按 `master` 的 ESP-IDF API 写的应用。

核心变化：**库自己打开 ALSA，公开 API 不再出现 `i2s_chan_handle_t`、`RingbufHandle_t`、
`esp_err_t`。**

---

## 1. 头文件

之前：

```c
#include "espaudiocore.h"
#include "driver/i2s_std.h"   /* linux_i2s_open / i2s_chan_handle_t */
```

现在：

```c
#include "espaudiocore.h"     /* 唯一需要包含的头文件 */
```

`espaudiocore.h` 自带 `<errno.h>`、`<stdint.h>`、`<stdbool.h>`，不再引入
ESP-IDF / FreeRTOS 头。

---

## 2. 播放一个文件

### 之前（兼容层）

```c
i2s_chan_handle_t tx = linux_i2s_open("plughw:0,0", 44100, 2, 16);
espaudiocore_cfg_t cfg = {0};
cfg.output_bits = 16;
cfg.on_event = on_event;

esp_err_t err = espaudiocore_begin("/music/a.mp3", tx, &cfg, &dec_err);
if (err != ESP_OK) {
    fprintf(stderr, "%s\n", esp_err_to_name(err));
}
...
espaudiocore_stop();
linux_i2s_close(tx);
```

### 现在

```c
espaudiocore_cfg_t cfg = {0};
cfg.alsa_device = "plughw:0,0";   /* 或 "default" / NULL */
cfg.output_bits = 16;
cfg.on_event = on_event;

int err = espaudiocore_begin("/music/a.mp3", &cfg, &dec_err);
if (err != 0) {
    fprintf(stderr, "%s\n", espaudiocore_strerror(err));
}
...
espaudiocore_stop();              /* 设备由库关闭 */
```

要点：

- `linux_i2s_open()` / `linux_i2s_close()` 删除。
- `tx` 参数删除；设备名放进 `cfg.alsa_device`。
- 采样率/位深/声道由解码器决定，库内部 `snd_pcm_set_params`；不需要预先按
  44100/2/16 打开。
- `esp_err_to_name()` → `espaudiocore_strerror()`。
- `ESP_OK` → `0`；`ESP_ERR_*` → `ESPAUDIOCORE_ERR_*`（负 errno）。

---

## 3. 错误处理速查

| 之前 | 现在 |
|---|---|
| `err == ESP_OK` | `err == 0` |
| `ESP_ERR_INVALID_ARG` | `ESPAUDIOCORE_ERR_INVALID_ARG` |
| `ESP_ERR_NOT_FOUND` | `ESPAUDIOCORE_ERR_NOT_FOUND` |
| `ESP_ERR_NOT_SUPPORTED` | `ESPAUDIOCORE_ERR_NOT_SUPPORTED` |
| `ESP_ERR_INVALID_RESPONSE` | `ESPAUDIOCORE_ERR_HEADER` |
| `ESP_ERR_INVALID_STATE` | `ESPAUDIOCORE_ERR_STATE` |
| `ESP_ERR_NO_MEM` | `ESPAUDIOCORE_ERR_NO_MEM` |
| `ESP_FAIL` | `ESPAUDIOCORE_ERR_IO` / `ESPAUDIOCORE_ERR_DECODER` |

`dec_err` 仍是解码器原生码，含义不变。

---

## 4. 回调

`on_event` / `on_meta` / `on_pcm` 的签名和语义不变，只是线程从 FreeRTOS task
变成 pthread：

- 仍然在解码线程上同步执行；
- 仍然禁止阻塞；
- `ESPAUDIOCORE_EVT_*` 枚举值不变；
- ringbuf 输出时 `on_pcm` 仍然不触发（应用已经拿到 PCM）。

---

## 5. 环形缓冲

之前用 FreeRTOS：

```c
RingbufHandle_t out = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);
espaudiocore_begin_rb(path, out, &cfg, &dec_err);
// xRingbufferReceive / vRingbufferReturnItem ...
vRingbufferDelete(out);
```

现在用库自带的 `espaudiocore_ringbuf_t`：

```c
espaudiocore_ringbuf_t *out = espaudiocore_ringbuf_create(64 * 1024);
espaudiocore_begin_rb(path, out, &cfg, &dec_err);

uint8_t buf[4096];
size_t n;
while (espaudiocore_is_running() || espaudiocore_ringbuf_used(out) > 0) {
    n = espaudiocore_ringbuf_read(out, buf, sizeof(buf), 100);
    if (n) fwrite(buf, 1, n, fp);
}

espaudiocore_stop();
espaudiocore_ringbuf_destroy(out);
```

映射：

| FreeRTOS | 本分支 |
|---|---|
| `xRingbufferCreate(size, BYTEBUF)` | `espaudiocore_ringbuf_create(size)` |
| `xRingbufferSend(rb, p, n, ticks)` | `espaudiocore_ringbuf_write(rb, p, n, timeout_ms)` |
| `xRingbufferReceiveUpTo(rb, &n, ticks, max)` | `espaudiocore_ringbuf_read(rb, p, max, timeout_ms)` |
| `vRingbufferReturnItem` | 不需要（read 已拷贝） |
| `vRingbufferDelete` | `espaudiocore_ringbuf_destroy` |
| `xRingbufferGetCurFreeSize` | `espaudiocore_ringbuf_space` |

`timeout_ms`：`<0` 无限，`0` 非阻塞，`>0` 毫秒。

---

## 6. ringbuf 源（流式输入）

之前：

```c
RingbufHandle_t in = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);
espaudiocore_begin_stream(in, ESPAUDIOCORE_FMT_MP3, tx, &cfg, &dec_err);
```

现在：

```c
espaudiocore_ringbuf_t *in = espaudiocore_ringbuf_create(32768);
espaudiocore_begin_stream(in, ESPAUDIOCORE_FMT_MP3, &cfg, &dec_err);
```

`fmt` 仍然必须正确给出；ringbuf 源仍然不支持 seek。Opus/Vorbis 仍要求可 seek
的源，因此不能作为 ringbuf 输入（与 master 一致）。

---

## 7. `hmi_shell` 的适配点

`hmi_shell/third_party/eac` 是 `master` + 兼容层的**预编译产物**，不能直接配
本分支的新头文件使用。接入时：

1. 用本分支重新构建 `libespaudiocore.a` 和 `include/espaudiocore.h`，
   覆盖 `third_party/eac/lib` 与 `third_party/eac/include`；
2. 删除 `third_party/eac/include/esp_err.h`、`esp_log.h`、`esp_heap_caps.h`、
   `sdkconfig.h`、`freertos/`、`driver/i2s_std.h`（新头文件不再包含它们）；
3. 修改调用点（grep `espaudiocore_` / `linux_i2s_`）：

| 文件 | 需要改的地方 |
|---|---|
| `player/tools/i2s_probe.c` | 删除 `linux_i2s_open/close`，`begin(path, &cfg, &dec_err)` |
| `third_party/music_demo/app/demo_audio.c` | 同上；`esp_err_t` → `int`；`ESP_OK` → `0` |
| `third_party/music_demo/lvgl/demos/music/lv_demo_music_main.c` | 只调 `seek_ms`/`get_*`，返回码比较改成 `0` |
| `hmi_shell/player/include/esp_err.h` | 仅 HMI 自己的服务空桩还在用；可以保留，但不要再传给 eac |

示例（`demo_audio.c`）：

```c
/* 之前 */
tx = linux_i2s_open("plughw:0,0", 44100, 2, 16);
err = espaudiocore_begin(path, tx, &cfg, &dec_err);

/* 现在 */
cfg.alsa_device = "plughw:0,0";
err = espaudiocore_begin(path, &cfg, &dec_err);   /* err: int, 0 = OK */
```

板端仍然需要 root 运行（`/dev/snd` 权限），与 HMI 的既有约定一致。

---

## 8. 不要做的事

- 不要把 `master` 的 `port/linux/include/*` 再加回本分支：新实现不引用
  `esp_err_t`、`xTaskCreate`、`i2s_channel_write`，加了也没有意义。
- 不要在回调里调用 `espaudiocore_stop()`：会返回 `ESPAUDIOCORE_ERR_STATE`，
  避免自 join 死锁。
- 不要在 `stop()` 之前销毁应用拥有的 ringbuf：解码线程仍可能在读写它。
