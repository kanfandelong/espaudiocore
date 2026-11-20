# espaudiocore -- native Linux build (CMake)
#
# Layers:
#   src/*.cpp              decoder core (routing, abstractions, player thread)
#   src/audio_port.c       Linux platform layer (logging, aligned memory)
#   lib/*                  vendored third-party decoders (compiled in-tree)
#   compat/                legacy AVR/Arduino headers used by those libraries
#
# The ESP-IDF component build lives on the master branch. This branch is
# Linux-only: pthreads, POSIX I/O and ALSA (dlopen, no ALSA headers needed).

cmake_minimum_required(VERSION 3.16)
project(espaudiocore VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ESPAUDIOCORE_ENABLE_WAV     "Enable WAV (PCM) decoder"        ON)
option(ESPAUDIOCORE_ENABLE_MP3     "Enable MP3 (libhelix-mp3)"       ON)
option(ESPAUDIOCORE_ENABLE_AAC     "Enable AAC (libhelix-aac, SBR)"  ON)
option(ESPAUDIOCORE_ENABLE_M4A     "Enable M4A (avpack + AAC)"       ON)
option(ESPAUDIOCORE_ENABLE_FLAC    "Enable FLAC (libflac)"           ON)
option(ESPAUDIOCORE_ENABLE_VORBIS  "Enable Ogg Vorbis (libvorbis)"   ON)
option(ESPAUDIOCORE_ENABLE_OPUS    "Enable Opus (libopus + opusfile)" ON)
option(ESPAUDIOCORE_ENABLE_WAVPACK "Enable WavPack (libwavpack)"     ON)
option(ESPAUDIOCORE_BUILD_TOOLS    "Build the eac_play command line tool" ON)
option(ESPAUDIOCORE_BUILD_TESTS    "Build the small self-test programs"   OFF)

if(ESPAUDIOCORE_ENABLE_M4A AND NOT ESPAUDIOCORE_ENABLE_AAC)
    message(FATAL_ERROR "M4A requires the AAC decoder; enable ESPAUDIOCORE_ENABLE_AAC or disable M4A")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Decoder core (always compiled; each decoder file self-gates on the macros)
# ---------------------------------------------------------------------------
set(core_srcs
    src/audio_port.c
    src/audio_ringbuf.cpp
    src/espaudiocore.cpp
    src/audio_player.cpp
    src/audio_decoder_route.cpp
    src/audio_source_fs.cpp
    src/audio_source_ringbuf.cpp
    src/audio_sink_alsa.cpp
    src/audio_sink_ringbuf.cpp
    src/id3_parser.cpp
    src/xing_parser.cpp
    src/decoder_wav.cpp
    src/decoder_mp3.cpp
    src/decoder_aac.cpp
    src/decoder_flac.cpp
    src/decoder_vorbis.cpp
    src/decoder_opus.cpp
    src/decoder_m4a.cpp
    src/decoder_wavpack.cpp
)

# ---------------------------------------------------------------------------
# Third-party decoders (selected by options)
# ---------------------------------------------------------------------------
set(tp_srcs)
set(tp_incs)
set(tp_defs)

if(ESPAUDIOCORE_ENABLE_M4A AND ESPAUDIOCORE_ENABLE_AAC)
    list(APPEND tp_incs "lib" "lib/avpack")
    list(APPEND tp_srcs
        "lib/ffbase/str-format.c"
        "lib/ffbase/sort.c"
    )
    list(APPEND tp_defs FF_UNIX FF_LITTLE_ENDIAN FFBASE_OPT_SIZE)
endif()

if(ESPAUDIOCORE_ENABLE_MP3)
    list(APPEND tp_srcs
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
    list(APPEND tp_incs "lib/libhelix-mp3")
    list(APPEND tp_defs USE_DEFAULT_STDLIB)
endif()

if(ESPAUDIOCORE_ENABLE_AAC)
    list(APPEND tp_srcs
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
    list(APPEND tp_incs "lib/libhelix-aac")
    list(APPEND tp_defs USE_DEFAULT_STDLIB)
endif()

if(ESPAUDIOCORE_ENABLE_FLAC)
    list(APPEND tp_srcs
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
    list(APPEND tp_incs "lib/libflac")
    list(APPEND tp_defs USE_DEFAULT_STDLIB)
endif()

if(ESPAUDIOCORE_ENABLE_VORBIS OR ESPAUDIOCORE_ENABLE_OPUS)
    list(APPEND tp_srcs
        "lib/libogg/bitwise.c"
        "lib/libogg/framing.c"
    )
    list(APPEND tp_incs "lib/libogg" "lib/libogg/ogg")
    list(APPEND tp_defs USE_DEFAULT_STDLIB)
endif()

if(ESPAUDIOCORE_ENABLE_VORBIS)
    list(APPEND tp_srcs
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
    list(APPEND tp_incs "lib/libvorbis")
endif()

if(ESPAUDIOCORE_ENABLE_OPUS)
    list(APPEND tp_srcs
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
        "lib/libopus/silk/NLSF_del_dec_quant.c"
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
    list(APPEND tp_incs "lib/libopus")
    list(APPEND tp_srcs
        "lib/opusfile/info.c"
        "lib/opusfile/internal.c"
        "lib/opusfile/opusfile.c"
        "lib/opusfile/stream.c"
    )
    list(APPEND tp_incs "lib/opusfile")
endif()

if(ESPAUDIOCORE_ENABLE_WAVPACK)
    list(APPEND tp_incs "lib/libwavpack")
    list(APPEND tp_srcs
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
# Library target
# ---------------------------------------------------------------------------
add_library(eac ${core_srcs} ${tp_srcs})
add_library(espaudiocore::eac ALIAS eac)
set_target_properties(eac PROPERTIES OUTPUT_NAME espaudiocore)

target_include_directories(eac
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        include/private
        src
        compat
        ${tp_incs}
)

target_compile_definitions(eac PRIVATE
    ESPAUDIOCORE_MAX_ID3TAG_MEM_SIZE=512
    ${tp_defs}
)

foreach(fmt WAV MP3 AAC M4A FLAC VORBIS OPUS WAVPACK)
    if(ESPAUDIOCORE_ENABLE_${fmt})
        target_compile_definitions(eac PRIVATE ESPAUDIOCORE_ENABLE_${fmt}=1)
    endif()
endforeach()

target_compile_options(eac PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)

# Vendored code is old and noisy: no warnings, keep the portable C paths.
if(tp_srcs)
    set_source_files_properties(${tp_srcs} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# avpack headers use C-style implicit conversions.
set_source_files_properties(src/decoder_m4a.cpp PROPERTIES COMPILE_OPTIONS "-fpermissive")

target_link_libraries(eac PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(ESPAUDIOCORE_BUILD_TOOLS)
    add_executable(eac_play examples/eac_play.c)
    target_link_libraries(eac_play PRIVATE eac)
    target_compile_options(eac_play PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(ESPAUDIOCORE_BUILD_TESTS)
    add_executable(eac_selftest tests/eac_selftest.c)
    target_link_libraries(eac_selftest PRIVATE eac)

    add_executable(pcm_compare tests/pcm_compare.c)
    target_link_libraries(pcm_compare PRIVATE m)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS eac
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/espaudiocore.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
