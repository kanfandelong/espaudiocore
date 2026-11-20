# 原生 Linux 解码验证记录

环境：

- 板卡：RK3506J，3×Cortex-A7，Linux 6.1，ALSA `plughw:0,0`
- 主机：WSL2 Ubuntu 24.04 / x86_64，用于交叉对比和 sanitizer
- 分支：`linux-native`，宿主/ARM 同一份源码
- 真实素材：板载 USB 存储中的 MP3/FLAC/M4A/OGG/Opus/AAC（2647 个 MP3、403 个 FLAC…）

本文件记录**真实音乐文件**上的解码正确性、性能、seek 与已知限制；合成正弦测试见
`tests/smoke.sh` / `tests/compare_pcm.sh`。

## 0. 真实文件抽检

从板载 USB 存储随机抽取 20 个 MP3、10 个 FLAC，加上全部 17 个 M4A 和全部
OGG/Opus/AAC 文件，共 **52 个真实文件**，在 RK3506 上用 ALSA `null` 完整解码：

```text
sample total=52 fail=0
```

即 52/52 全部正常进入 EOS，无解码失败。下面的表格是其中代表性文件的精确对比。

---

## 1. 正确性：无损格式

FLAC 与 `flac -d --force-raw-format` 的参考 PCM 逐字节比较：

| 文件 | 参数 | 解码字节数 | 结果 |
|---|---|---|---|
| `Elektronomia - Collide_SQ.flac` | 44.1 kHz / 2ch / 16-bit / 221.5 s | 39,080,232 | **IDENTICAL** |
| `夜与破晓的誓刃.flac`（文件被截断） | 48 kHz / 2ch / 24-bit / 头部 190.0 s，实际 106.15 s | 30,572,544 | **IDENTICAL**（与 `flac` 的部分解码结果一致） |

说明：第二个文件本身不完整（STREAMINFO 声明 9120379 samples，文件只到约 5.1M
samples），`flac` 自己也会报 `ERROR while decoding data`，但两者输出的 PCM 完全一致。
本库会记录 `LOST_SYNC/BAD_HEADER` 警告后按 EOS 结束，`get_duration_ms()` 仍返回头部
声明的 190007 ms。

---

## 2. 正确性：有损格式（对比 ffmpeg）

两路 PCM 都解码为 s16le 后按采样点比较（`tests/pcm_compare.c`）。
`max_diff` 是最大绝对样点差（满量程 32767），SNR 以 ffmpeg 输出为参考。

| 文件 | 格式/参数 | 采样数（本库/参考） | max_diff | SNR | 相关系数 |
|---|---|---|---|---|---|
| `pno-cs.mp3` | MP3 48 kHz 20.040 s | 1,923,840 / 1,923,840 | 1 | 82.15 dB | 1.000000 |
| `「未行之路」空_荧版.mp3` | MP3 44.1 kHz 85.0 s | 7,494,912 / 7,497,216 | 1 | 87.80 dB | 1.000000 |
| `「轻涟」唢呐版.aac` | AAC-LC 48 kHz 149.120 s | 14,315,520 / 14,315,520 | 2251 | 54.85 dB | 0.999998 |
| `【2026茜特菈莉生日会】和你.opus`（199 s 片段） | Opus 48 kHz | 19,101,456 / 19,101,456 | 18 | 70.31 dB | 1.000000 |

结论：

- MP3 与 ffmpeg 的差异只有 ±1 LSB（不同解码器的舍入），可视为听感一致；
- AAC 是不同实现（helix vs ffmpeg），相关系数 0.999998、SNR 54.85 dB，长度完全一致；
- Opus 长度完全一致，最大差 18 LSB。

第二个 MP3 少 1152 个采样点（一个 MP3 帧，26 ms），是无 Xing/Info 头的文件在
EOF 处丢最后一帧；静音/尾帧场景影响很小。

### 跨架构一致性

同一文件在 x86_64 主机与 RK3506 ARM 上解码，raw PCM 的 SHA-256 完全相同：

| 文件 | SHA-256 |
|---|---|
| `pno-cs.mp3` | `64e186bc…204e8a` |
| `「未行之路」空_荧版.mp3` | `d6f5e998…f4611434` |
| `「轻涟」唢呐版.aac` | `7d6002a6…b03614b` |

---

## 3. 性能（NULL ALSA 输出，纯解码 + 写出）

NULL 设备不做实时节流，因此 `elapsed` 近似纯 CPU 解码时间；`speed = duration / elapsed`。

| 文件 | 时长 | 解码耗时 | 实时倍率 |
|---|---|---|---|
| `中文测试.mp3`（44.1k） | 155.454 s | 2383 ms | 65.2× |
| `pno-cs.mp3`（48k） | 20.040 s | 373 ms | 53.7× |
| `「未行之路」…mp3`（44.1k） | 84.976 s | 2127 ms | 40.0× |
| `Elektronomia - Collide_SQ.flac`（16-bit） | 221.543 s | 9056 ms | 24.5× |
| 24-bit/48k FLAC（实际 106.15 s） | 106.154 s | 5440 ms | 19.5× |
| `「轻涟」唢呐版.aac`（48k） | 149.120 s | 2278 ms | 65.5× |
| `【…】和你.m4a`（48k） | 237.035 s | 5790 ms | 40.9× |
| `【…】和你.ogg`（48k） | 237.013 s | 4990 ms | 47.5× |
| `【…】和你.opus`（48k） | 237.013 s | 12810 ms | 18.5× |
| Blue Archive `01 - Quintilis Abydos.m4a` | 125.525 s | 2683 ms | 46.8× |

最慢的 Opus 也有 18.5× 实时余量；真实 ALSA 播放时未观察到 underrun。

---

## 4. 真实 ALSA 播放

| 文件 | 设备/参数 | 结果 |
|---|---|---|
| 24-bit/48k FLAC | `plughw:0,0`，3 s | 位置 1.0 s→2.0 s→3.0 s，正常停止 |
| 16-bit/44.1k FLAC，seek 60 s | `plughw:0,0`，3 s | 60.0 s→63.0 s，seek 后丢弃旧缓冲 |
| MP3 48k | `plughw:0,0`，3 s | 位置按实时推进 |
| Opus 48k / OGG 48k | `plughw:0,0`，3 s | 位置按实时推进 |

---

## 5. seek 支持矩阵（真实文件）

| 格式 | seek 60 s | 备注 |
|---|---|---|
| FLAC | ✅ | 采样级精确 |
| M4A | ✅ | 容器索引 |
| OGG Vorbis | ✅ | 容器索引 |
| Opus | ✅ | 容器索引 |
| MP3（有 Xing/Info） | ✅ | `中文测试.mp3` 落到 63.2 s |
| MP3（无 Xing/Info） | ❌ `rc=4` | 无 TOC、无平均码率，无法定位 |
| AAC（ADTS） | ❌ `rc=4` | 纯顺序流，无索引 |

`rc=4` 对应解码器内部 `AUDIO_ERR_IO`；`espaudiocore_seek_ms()` 会把它映射为负
错误码返回。播放不受影响，只是不能跳转。

---

## 6. 内存/未定义行为检查

- **AddressSanitizer**（10 个合成文件 + 4 个真实文件 + selftest）：
  未发现 use-after-free / buffer overflow / leak。
- **UBSan**：本库自有代码在修复了 PCM 对齐中的有符号左移后不再报告；第三方库仍有
  大量 `left shift of negative value`、AVPack 的 misaligned load 等历史问题，属于
  上游实现，输出正确且已在 master 上存在。

---

## 7. 已知限制（非本分支引入）

1. **无 Xing/Info 的 MP3**：`duration=-1`、seek 失败；解码长度正确（可能少最后一帧）。
2. **ADTS AAC**：duration 未知、seek 不支持；实际解码长度正确（例如
   `「轻涟」唢呐版.aac` 为 149.12 s，ffprobe 的 140.45 s 是估算误差）。
3. **被截断的 FLAC**：`get_duration_ms()` 使用 STREAMINFO 的总样本数，可能与实际
   数据长度不一致；本库按 EOS 结束并打印警告，参考 `flac` 同样只能解出部分数据。
4. 真实 WavPack 文件未找到，WavPack 只做了合成文件验证。
5. 第三方库的 UBSan 报告不影响当前 GCC/ARM 的结果，但严格来说属于上游 UB。

---

## 8. 复现

```bash
./scripts/build.sh
./scripts/build.sh --cross

# 合成素材
./tests/gen_assets.sh /path/to/ffmpeg tests/assets
./tests/smoke.sh ~/espaudiocore-linux/build-host/eac_play tests/assets
./tests/compare_pcm.sh ~/espaudiocore-linux/build-host/eac_play tests/assets

# 有损格式对比（需要自己准备 raw PCM 参考）
~/espaudiocore-linux/build-host/pcm_compare ours.pcm ref.pcm 2 48000

# 板端真实文件
./scripts/board_test.sh --alsa
```

`eac_play` 新增 `--seek <ms>`；`--seconds` 现在表示“从 seek 点起再播放 N 秒”。
