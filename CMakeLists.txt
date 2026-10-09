# espaudiocore —— ESP-IDF 组件构建脚本
#
# 分层：
#   src/*.c                     解码核心（路由、抽象、任务）—— 本项目自有代码
#   src/port/*.c                平台适配层（日志/内存/pgmspace shim）
#   lib/*                       第三方解码库 —— 原样保留，仅在本项目内编译
#
# 注意：第三方库的 include 路径逐库隔离，避免同名头文件（config.h / config_types.h）
#       互相串台。这也是不把 lib/ 做成独立组件的替代方案。

idf_build_get_property(target IDF_TARGET)

# ---------------------------------------------------------------------------
# 自有源码
# ---------------------------------------------------------------------------
set(srcs
    "src/espaudiocore.cpp"
    "src/audio_player.cpp"
    "src/audio_decoder_route.cpp"
    "src/audio_source_fs.cpp"
    "src/audio_source_ringbuf.cpp"
    "src/audio_sink_i2s.cpp"
    "src/audio_sink_ringbuf.cpp"
    "src/port/audio_port.c"
    "src/id3_parser.cpp"
    "src/xing_parser.cpp"
    "src/decoder_wav.cpp"
    "src/decoder_mp3.cpp"
    "src/decoder_aac.cpp"
    "src/decoder_flac.cpp"
    "src/decoder_vorbis.cpp"
    "src/decoder_opus.cpp"
    "src/decoder_m4a.cpp"
    "src/decoder_wavpack.cpp"
)

if(CONFIG_ESPAUDIOCORE_ENABLE_M4A AND CONFIG_ESPAUDIOCORE_ENABLE_AAC)
    set_source_files_properties("src/decoder_m4a.cpp" PROPERTIES
        COMPILE_DEFINITIONS "FF_LITTLE_ENDIAN;FF_UNIX")
endif()

# ---------------------------------------------------------------------------
# 第三方解码库（默认全部编译；可在 menuconfig 里按格式裁剪）
# ---------------------------------------------------------------------------
set(thirdparty_srcs)
set(thirdparty_incs)
set(thirdparty_flags)

list(APPEND thirdparty_incs "lib" "lib/avpack")
list(APPEND thirdparty_incs "lib" "lib/ffbase")
list(APPEND thirdparty_srcs
    "lib/ffbase/str-format.c"
    "lib/ffbase/sort.c"
)

if(CONFIG_ESPAUDIOCORE_ENABLE_MP3)
    list(APPEND thirdparty_srcs
        "lib/libhelix-mp3/bitstream.c"
        "lib/libhelix-mp3/buffers.c"
        "lib/libhelix-mp3/dct32.c"
        "lib/libhelix-mp3/dequant.c"
        "lib/libhelix-mp3/dqchan.c"
        "lib/libhelix-mp3/huffman.c"
        "lib/libhelix-mp3/hufftabs.c"
        "lib/libhelix-mp3/imdct.c"
        "lib/libhelix-mp3/mp3dec.c"
        "lib/libhelix-mp3/mp3tabs.c"
        "lib/libhelix-mp3/polyphase.c"
        "lib/libhelix-mp3/scalfact.c"
        "lib/libhelix-mp3/stproc.c"
        "lib/libhelix-mp3/subband.c"
        "lib/libhelix-mp3/trigtabs.c"
    )
    list(APPEND thirdparty_incs "lib/libhelix-mp3")
    list(APPEND thirdparty_flags "-DUSE_DEFAULT_STDLIB")
endif()

if(CONFIG_ESPAUDIOCORE_ENABLE_AAC)
    list(APPEND thirdparty_srcs
        "lib/libhelix-aac/aacdec.c"
        "lib/libhelix-aac/aactabs.c"
        "lib/libhelix-aac/bitstream.c"
        "lib/libhelix-aac/buffers.c"
        "lib/libhelix-aac/dct4.c"
        "lib/libhelix-aac/decelmnt.c"
        "lib/libhelix-aac/dequant.c"
        "lib/libhelix-aac/fft.c"
        "lib/libhelix-aac/filefmt.c"
        "lib/libhelix-aac/huffman.c"
        "lib/libhelix-aac/hufftabs.c"
        "lib/libhelix-aac/imdct.c"
        "lib/libhelix-aac/noiseless.c"
        "lib/libhelix-aac/pns.c"
        "lib/libhelix-aac/sbr.c"
        "lib/libhelix-aac/sbrfft.c"
        "lib/libhelix-aac/sbrfreq.c"
        "lib/libhelix-aac/sbrhfadj.c"
        "lib/libhelix-aac/sbrhfgen.c"
        "lib/libhelix-aac/sbrhuff.c"
        "lib/libhelix-aac/sbrimdct.c"
        "lib/libhelix-aac/sbrmath.c"
        "lib/libhelix-aac/sbrqmf.c"
        "lib/libhelix-aac/sbrside.c"
        "lib/libhelix-aac/sbrtabs.c"
        "lib/libhelix-aac/stproc.c"
        "lib/libhelix-aac/tns.c"
        "lib/libhelix-aac/trigtabs.c"
    )
    list(APPEND thirdparty_incs "lib/libhelix-aac")
    list(APPEND thirdparty_flags "-DUSE_DEFAULT_STDLIB")
endif()

if(CONFIG_ESPAUDIOCORE_ENABLE_FLAC)
    list(APPEND thirdparty_srcs
        "lib/libflac/bitmath.c"
        "lib/libflac/bitreader.c"
        "lib/libflac/cpu.c"
        "lib/libflac/crc.c"
        "lib/libflac/fixed.c"
        "lib/libflac/float.c"
        "lib/libflac/format.c"
        "lib/libflac/lpc.c"
        "lib/libflac/md5.c"
        "lib/libflac/memory.c"
        "lib/libflac/stream_decoder.c"
        "lib/libflac/window.c"
    )
    # libflac 的 "config.h"/"private/*" 在库根，"FLAC/*"/"share/*" 也在库根下
    list(APPEND thirdparty_incs "lib/libflac")
    list(APPEND thirdparty_flags "-DUSE_DEFAULT_STDLIB")
    # ESP32-S3 的 xtensa LPC 汇编：仅在 libflac/config.h 里 EN_ESP_ASM 打开时需要
    if(CONFIG_ESPAUDIOCORE_FLAC_XTENSA_ASM AND CONFIG_IDF_TARGET_ESP32S3)
        list(APPEND thirdparty_srcs "lib/libflac/lpc_intrin_xtensa.c")
    endif()
endif()

if(CONFIG_ESPAUDIOCORE_ENABLE_VORBIS OR CONFIG_ESPAUDIOCORE_ENABLE_OPUS)
    list(APPEND thirdparty_srcs
        "lib/libogg/bitwise.c"
        "lib/libogg/framing.c"
    )
    list(APPEND thirdparty_incs "lib/libogg" "lib/libogg/ogg")
    list(APPEND thirdparty_flags "-DUSE_DEFAULT_STDLIB")
endif()

if(CONFIG_ESPAUDIOCORE_ENABLE_VORBIS)
    list(APPEND thirdparty_srcs
        "lib/libvorbis/block.c"
        "lib/libvorbis/codebook.c"
        "lib/libvorbis/floor0.c"
        "lib/libvorbis/floor1.c"
        "lib/libvorbis/info.c"
        "lib/libvorbis/mapping0.c"
        "lib/libvorbis/mdct.c"
        "lib/libvorbis/registry.c"
        "lib/libvorbis/res012.c"
        "lib/libvorbis/sharedbook.c"
        "lib/libvorbis/synthesis.c"
        "lib/libvorbis/vorbisfile.c"
        "lib/libvorbis/window.c"
    )
    list(APPEND thirdparty_incs "lib/libvorbis")
endif()

if(CONFIG_ESPAUDIOCORE_ENABLE_OPUS)
    list(APPEND thirdparty_srcs
        "lib/libopus/opus.c"
        "lib/libopus/opus_decoder.c"
        "lib/libopus/opus_multistream.c"
        "lib/libopus/opus_multistream_decoder.c"
        "lib/libopus/repacketizer.c"
        "lib/libopus/celt/bands.c"
        "lib/libopus/celt/celt.c"
        "lib/libopus/celt/celt_decoder.c"
        "lib/libopus/celt/celt_lpc.c"
        "lib/libopus/celt/cwrs.c"
        "lib/libopus/celt/entcode.c"
        "lib/libopus/celt/entdec.c"
        "lib/libopus/celt/entenc.c"
        "lib/libopus/celt/kiss_fft.c"
        "lib/libopus/celt/laplace.c"
        "lib/libopus/celt/mathops.c"
        "lib/libopus/celt/mdct.c"
        "lib/libopus/celt/modes.c"
        "lib/libopus/celt/pitch.c"
        "lib/libopus/celt/quant_bands.c"
        "lib/libopus/celt/rate.c"
        "lib/libopus/celt/vq.c"
        "lib/libopus/silk/A2NLSF.c"
        "lib/libopus/silk/CNG.c"
        "lib/libopus/silk/LPC_fit.c"
        "lib/libopus/silk/LPC_inv_pred_gain.c"
        "lib/libopus/silk/PLC.c"
        "lib/libopus/silk/NLSF2A.c"
        "lib/libopus/silk/NLSF_decode.c"
        "lib/libopus/silk/NLSF_stabilize.c"
        "lib/libopus/silk/NLSF_unpack.c"
        "lib/libopus/silk/NLSF_VQ.c"
        "lib/libopus/silk/NLSF_VQ_weights_laroia.c"
        "lib/libopus/silk/ana_filt_bank_1.c"
        "lib/libopus/silk/biquad_alt.c"
        "lib/libopus/silk/bwexpander.c"
        "lib/libopus/silk/bwexpander_32.c"
        "lib/libopus/silk/check_control_input.c"
        "lib/libopus/silk/code_signs.c"
        "lib/libopus/silk/control_audio_bandwidth.c"
        "lib/libopus/silk/control_codec.c"
        "lib/libopus/silk/control_SNR.c"
        "lib/libopus/silk/dec_API.c"
        "lib/libopus/silk/decode_core.c"
        "lib/libopus/silk/decode_frame.c"
        "lib/libopus/silk/decode_indices.c"
        "lib/libopus/silk/decode_parameters.c"
        "lib/libopus/silk/decode_pitch.c"
        "lib/libopus/silk/decode_pulses.c"
        "lib/libopus/silk/decoder_set_fs.c"
        "lib/libopus/silk/gain_quant.c"
        "lib/libopus/silk/init_decoder.c"
        "lib/libopus/silk/inner_prod_aligned.c"
        "lib/libopus/silk/interpolate.c"
        "lib/libopus/silk/lin2log.c"
        "lib/libopus/silk/log2lin.c"
        "lib/libopus/silk/LPC_analysis_filter.c"
        "lib/libopus/silk/LP_variable_cutoff.c"
        "lib/libopus/silk/NLSF_encode.c"
        "lib/libopus/silk/NSQ.c"
        "lib/libopus/silk/NSQ_del_dec.c"
        "lib/libopus/silk/pitch_est_tables.c"
        "lib/libopus/silk/process_NLSFs.c"
        "lib/libopus/silk/quant_LTP_gains.c"
        "lib/libopus/silk/resampler.c"
        "lib/libopus/silk/resampler_down2.c"
        "lib/libopus/silk/resampler_down2_3.c"
        "lib/libopus/silk/resampler_private_AR2.c"
        "lib/libopus/silk/resampler_private_down_FIR.c"
        "lib/libopus/silk/resampler_private_IIR_FIR.c"
        "lib/libopus/silk/resampler_private_up2_HQ.c"
        "lib/libopus/silk/resampler_rom.c"
        "lib/libopus/silk/shell_coder.c"
        "lib/libopus/silk/sigm_Q15.c"
        "lib/libopus/silk/sort.c"
        "lib/libopus/silk/stereo_decode_pred.c"
        "lib/libopus/silk/stereo_MS_to_LR.c"
        "lib/libopus/silk/sum_sqr_shift.c"
        "lib/libopus/silk/table_LSF_cos.c"
        "lib/libopus/silk/tables_gain.c"
        "lib/libopus/silk/tables_LTP.c"
        "lib/libopus/silk/tables_NLSF_CB_NB_MB.c"
        "lib/libopus/silk/tables_NLSF_CB_WB.c"
        "lib/libopus/silk/tables_other.c"
        "lib/libopus/silk/tables_pitch_lag.c"
        "lib/libopus/silk/tables_pulses_per_block.c"
        "lib/libopus/silk/VAD.c"
        "lib/libopus/silk/VQ_WMat_EC.c"
        "lib/libopus/silk/debug.c"
    )
    list(APPEND thirdparty_incs "lib/libopus")
    # opusfile（OGG 封装 + 标签解析）
    list(APPEND thirdparty_srcs
        "lib/opusfile/info.c"
        "lib/opusfile/internal.c"
        "lib/opusfile/opusfile.c"
        "lib/opusfile/stream.c"
    )
    list(APPEND thirdparty_incs "lib/opusfile")
endif()

if (CONFIG_ESPAUDIOCORE_ENABLE_WAVPACK)
    list(APPEND thirdparty_incs "lib/libwavpack")
    list(APPEND thirdparty_srcs
        "lib/libwavpack/common_utils.c"
        "lib/libwavpack/decorr_utils.c"
        "lib/libwavpack/open_legacy.c"
        "lib/libwavpack/read_words.c"
        "lib/libwavpack/unpack.c"
        "lib/libwavpack/unpack_utils.c"
        "lib/libwavpack/unpack3_open.c"
        "lib/libwavpack/entropy_utils.c"
        "lib/libwavpack/open_raw.c"
        "lib/libwavpack/tag_utils.c"
        "lib/libwavpack/unpack_floats.c"
        "lib/libwavpack/unpack3.c"
        "lib/libwavpack/unpack3_seek.c"
        "lib/libwavpack/open_filename.c"
        "lib/libwavpack/open_utils.c"
        "lib/libwavpack/tags.c"
        "lib/libwavpack/unpack_seek.c"
    )
endif()

# ---------------------------------------------------------------------------
# 注册组件
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS ${srcs} ${thirdparty_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/private" "src" "src/port" ${thirdparty_incs}
    REQUIRES esp_driver_i2s esp_driver_gpio esp_ringbuf
)

# 可选：强制文件数据源模式，便于在一次构建里对照验证两条路径。
#   idf.py -DESPAUDIOCORE_FILE_SOURCE_VFS_TEST=ON -B build_vfs build
# 优先级高于 Kconfig（见 src/audio_source_fs.cpp 顶部的模式选择）。
option(ESPAUDIOCORE_FILE_SOURCE_VFS_TEST "Force pure-VFS file source (setvbuf path verification)" OFF)
if(ESPAUDIOCORE_FILE_SOURCE_VFS_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPAUDIOCORE_FORCE_FILE_SOURCE_VFS=1)
    message(STATUS "espaudiocore: forcing PURE VFS file source")
endif()

option(ESPAUDIOCORE_FILE_SOURCE_WINDOW_TEST "Force self-managed window file source" OFF)
if(ESPAUDIOCORE_FILE_SOURCE_WINDOW_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPAUDIOCORE_FORCE_FILE_SOURCE_WINDOW=1)
    message(STATUS "espaudiocore: forcing self-managed WINDOW file source")
endif()

# 第三方库降级警告：它们是移植过来的老代码，不改动，避免淹没自有代码的告警。
# 同时强制注入 pgmspace 兼容层：IDF 不提供 pgmspace.h，而多个第三方库依赖它。
if(thirdparty_srcs)
    set_source_files_properties(${thirdparty_srcs} PROPERTIES COMPILE_OPTIONS
        "-w;-Wno-error;-include;esp_pgmspace.h;${thirdparty_flags}")
endif()

# 自有代码：按项目规范开告警
set_source_files_properties(${srcs} PROPERTIES COMPILE_OPTIONS
    "-Wall;-Wextra;-Wno-unused-parameter;-Wno-error")
