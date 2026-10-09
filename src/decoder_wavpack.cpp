/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_wavpack.cpp
 * @brief   WAVPACK 解码器
 */

#include "audio_port.h"
#include "audio_source.h"
#include "audio_types.h"
#include "decoder.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C" {
#include "wavpack.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_WAVPACK)

#define WAVPACK_BLOCK_FRAMES 1024

class DecoderWavPack : public AudioDecoder
{
public:
	~DecoderWavPack() override
	{
		close();
	}

	audio_err_t open(AudioInput *in, AudioSink *out, espaudiocore_meta_cb_t meta_cb,
					 void *meta_user, audio_format_t *fmt, int64_t *duration_ms,
					 int *dec_err) override
	{
		close();
		in_ = in;
		out_ = out;
		meta_cb_ = meta_cb;
		meta_user_ = meta_user;
		last_error_ = AUDIO_DEC_ERR_NONE;
		if (dec_err) {
			*dec_err = AUDIO_DEC_ERR_NONE;
		}
		if (duration_ms) {
			*duration_ms = -1;
		}
		if (!in_ || !out_) {
			return AUDIO_ERR_INVALID_ARG;
		}

		main_reader_.input = in_;
		correction_reader_.input = nullptr;
		if (in_->path && in_->path[0]) {
			char *correction_path = make_correction_path(in_->path);
			if (!correction_path) {
				if (dec_err) {
					*dec_err = AUDIO_DEC_ERR_OPEN;
				}
				close();
				return AUDIO_ERR_NO_MEM;
			}
			struct stat st;
			if (stat(correction_path, &st) == 0) {
				correction_in_ = audio_source_fs_create(correction_path);
			}
			free(correction_path);
			correction_reader_.input = correction_in_;
		}

		reader_.read_bytes = read_cb;
		reader_.write_bytes = nullptr;
		reader_.get_pos = get_pos_cb;
		reader_.set_pos_abs = set_pos_abs_cb;
		reader_.set_pos_rel = set_pos_rel_cb;
		reader_.push_back_byte = push_back_cb;
		reader_.get_length = get_length_cb;
		reader_.can_seek = can_seek_cb;
		reader_.truncate_here = nullptr;
		reader_.close = nullptr;

		int flags = OPEN_2CH_MAX;
		if (in_->can_seek()) {
			flags |= OPEN_TAGS;
		} else {
			flags |= OPEN_STREAMING;
		}
		if (correction_in_) {
			flags |= OPEN_WVC;
            AUDIO_LOGI("The WVC file has been added to the decoder");
		}
		char error[128] = {};
		wpc_ = WavpackOpenFileInputEx64(&reader_, &main_reader_,
										correction_in_ ? &correction_reader_ : nullptr,
										error, flags, 0);
		if (!wpc_) {
			AUDIO_LOGE("WavPack open failed: %s", error);
			if (dec_err) {
				*dec_err = AUDIO_DEC_ERR_OPEN;
			}
			close();
			return AUDIO_ERR_PARSE;
		}

		if (WavpackGetMode(wpc_) & MODE_FLOAT) {
			AUDIO_LOGE("WavPack floating-point samples are not supported");
			if (dec_err) {
				*dec_err = AUDIO_DEC_ERR_OPEN;
			}
			close();
			return AUDIO_ERR_NOT_SUPPORTED;
		}

		src_bits_ = WavpackGetBitsPerSample(wpc_);
		channels_ = WavpackGetReducedChannels(wpc_);
		fmt_.rate = WavpackGetSampleRate(wpc_);
		fmt_.channels = 2;
		fmt_.bits = (uint8_t)((src_bits_ <= 16) ? 16 : src_bits_);
		if (src_bits_ < 1 || src_bits_ > 32 || channels_ < 1 || channels_ > 2 || !fmt_.rate) {
			AUDIO_LOGE("WavPack has unsupported format: %d bits, %d channels, %u Hz",
					   src_bits_, channels_, (unsigned)fmt_.rate);
			if (dec_err) {
				*dec_err = AUDIO_DEC_ERR_PARSE;
			}
			close();
			return AUDIO_ERR_NOT_SUPPORTED;
		}

		sample_buf_ = (int32_t *)audio_alloc_big(WAVPACK_BLOCK_FRAMES * channels_ * sizeof(int32_t), 0);
		pcm32_ = (int32_t *)audio_alloc_big(WAVPACK_BLOCK_FRAMES * 2 * sizeof(int32_t), 0);
		if (!sample_buf_ || !pcm32_) {
			if (dec_err) {
				*dec_err = AUDIO_DEC_ERR_OPEN;
			}
			close();
			return AUDIO_ERR_NO_MEM;
		}

		int64_t samples = WavpackGetNumSamples64(wpc_);
		if (samples >= 0) {
			duration_hint_ms_ = (int64_t)((uint64_t)samples * 1000ull / fmt_.rate);
			report_duration(duration_hint_ms_);
		}
		report_tags();

		if (fmt) {
			*fmt = fmt_;
		}
		out_->set_rate(fmt_.rate);
		out_->set_format(fmt_.bits, fmt_.channels);
		if (duration_ms) {
			*duration_ms = duration_hint_ms_;
		}
		state_ = AUDIO_DEC_STATE_ACTIVE;
		AUDIO_LOGI("WavPack: %u Hz, %u ch, %u-bit%s", (unsigned)fmt_.rate,
				   (unsigned)fmt_.channels, (unsigned)src_bits_,
				   (WavpackGetMode(wpc_) & MODE_WVC) ? " + WVC" : "");
		return AUDIO_OK;
	}

	audio_err_t decode() override
	{
		if (state_ != AUDIO_DEC_STATE_ACTIVE || !wpc_) {
			return AUDIO_ERR_EOF;
		}

		uint32_t frames = WavpackUnpackSamples(wpc_, sample_buf_, WAVPACK_BLOCK_FRAMES);
		if (!frames) {
			state_ = AUDIO_DEC_STATE_EOS;
			return AUDIO_ERR_EOF;
		}

		const unsigned shift = 32u - (unsigned)src_bits_;
		const int64_t scale = (int64_t)1 << shift;
		for (uint32_t i = 0; i < frames; ++i) {
			for (int ch = 0; ch < channels_; ++ch) {
				int32_t value = (int32_t)((int64_t)sample_buf_[i * channels_ + ch] * scale);
				pcm32_[i * 2 + ch] = value;
				if (channels_ == 1) {
					pcm32_[i * 2 + 1] = value;
				}
			}
		}

		int rc = out_->write_i32(pcm32_, frames, fmt_.bits, 0);
		if (rc == AUDIO_ERR_NOT_SUPPORTED) {
			if (fmt_.bits != 16) {
				state_ = AUDIO_DEC_STATE_ERROR;
				last_error_ = AUDIO_ERR_NOT_SUPPORTED;
				return AUDIO_ERR_NOT_SUPPORTED;
			}
			if (!pcm16_) {
				pcm16_ = (int16_t *)audio_alloc_big(WAVPACK_BLOCK_FRAMES * 2 * sizeof(int16_t), 0);
				if (!pcm16_) {
					state_ = AUDIO_DEC_STATE_ERROR;
					last_error_ = AUDIO_ERR_NO_MEM;
					return AUDIO_ERR_NO_MEM;
				}
			}
			for (uint32_t i = 0; i < frames * 2; ++i) {
				pcm16_[i] = (int16_t)(pcm32_[i] >> 16);
			}
			rc = out_->write(pcm16_, (size_t)frames * 2 * sizeof(int16_t), 0);
		}
		if (rc != AUDIO_OK) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = rc;
			return (audio_err_t)rc;
		}
		return AUDIO_OK;
	}

    audio_err_t seek_ms(int64_t ms) override
    {
        uint64_t sample = (uint64_t)ms * fmt_.rate / 1000ull;
        return WavpackSeekSample64(wpc_, sample) == true ? AUDIO_OK : AUDIO_FAIL;
    }

	void reset() override
	{
        /* seek 由库的api完成；这里不用做任何事） */
		// if (wpc_ && in_ && in_->can_seek()) {
		// 	(void)WavpackSeekSample64(wpc_, 0);
		// }
		state_ = AUDIO_DEC_STATE_ACTIVE;
	}

	void close() override
	{
		if (wpc_) {
			WavpackCloseFile(wpc_);
			wpc_ = nullptr;
		}
		delete correction_in_;
		correction_in_ = nullptr;
		audio_free(sample_buf_);
		sample_buf_ = nullptr;
		audio_free(pcm32_);
		pcm32_ = nullptr;
		audio_free(pcm16_);
		pcm16_ = nullptr;
		in_ = nullptr;
		out_ = nullptr;
		meta_cb_ = nullptr;
		meta_user_ = nullptr;
		channels_ = 0;
		src_bits_ = 0;
		duration_hint_ms_ = -1;
		main_reader_ = {};
		correction_reader_ = {};
		reader_ = {};
		state_ = AUDIO_DEC_STATE_IDLE;
	}

private:
	struct InputReaderContext {
		AudioInput *input = nullptr;
		int pushed_byte = -1;
	};

	static char *make_correction_path(const char *path)
	{
		const char *slash = strrchr(path, '/');
		const char *dot = strrchr(path, '.');
		size_t base_len = (dot && (!slash || dot > slash)) ? (size_t)(dot - path) : strlen(path);
		char *result = (char *)malloc(base_len + sizeof(".wvc"));
		if (!result) {
			return nullptr;
		}
		memcpy(result, path, base_len);
		memcpy(result + base_len, ".wvc", sizeof(".wvc"));
		return result;
	}

	static int32_t read_cb(void *id, void *data, int32_t count)
	{
		InputReaderContext *context = static_cast<InputReaderContext *>(id);
		if (count <= 0) {
			return 0;
		}
		int32_t total = 0;
		if (context->pushed_byte >= 0) {
			((uint8_t *)data)[0] = (uint8_t)context->pushed_byte;
			context->pushed_byte = -1;
			total = 1;
		}
		if (total < count) {
			int got = context->input->read((uint8_t *)data + total, (size_t)(count - total));
			if (got < 0) {
				return total ? total : -1;
			}
			total += got;
		}
		return total;
	}

	static int64_t get_pos_cb(void *id)
	{
		InputReaderContext *context = static_cast<InputReaderContext *>(id);
		int64_t pos = context->input->tell();
		return (context->pushed_byte >= 0 && pos >= 0) ? pos - 1 : pos;
	}

	static int set_pos_abs_cb(void *id, int64_t pos)
	{
		InputReaderContext *context = static_cast<InputReaderContext *>(id);
		if (context->input->seek(pos, SEEK_SET) != AUDIO_OK) {
			return 0;
		}
		context->pushed_byte = -1;
		return 1;
	}

	static int set_pos_rel_cb(void *id, int64_t delta, int mode)
	{
		InputReaderContext *context = static_cast<InputReaderContext *>(id);
		if (context->pushed_byte >= 0 && mode == SEEK_CUR) {
			--delta;
		}
		if (context->input->seek(delta, mode) != AUDIO_OK) {
			return 0;
		}
		context->pushed_byte = -1;
		return 1;
	}

	static int push_back_cb(void *id, int value)
	{
		InputReaderContext *context = static_cast<InputReaderContext *>(id);
		if (context->pushed_byte >= 0) {
			return EOF;
		}
		context->pushed_byte = value & 0xff;
		return value;
	}

	static int64_t get_length_cb(void *id)
	{
		return static_cast<InputReaderContext *>(id)->input->size();
	}

	static int can_seek_cb(void *id)
	{
		return static_cast<InputReaderContext *>(id)->input->can_seek();
	}

	void report_duration(int64_t milliseconds)
	{
		if (!meta_cb_) {
			return;
		}
		char value[24];
		snprintf(value, sizeof(value), "%lld", (long long)milliseconds);
		espaudiocore_meta_t meta = {};
		meta.type = "TLEN";
		meta.data = value;
		meta.len = strlen(value);
		meta.pic_type = -1;
		meta_cb_(meta_user_, &meta);
	}

	void report_tags()
	{
		if (!meta_cb_) {
			return;
		}
		const int count = WavpackGetNumTagItems(wpc_);
		for (int i = 0; i < count; ++i) {
			int key_len = WavpackGetTagItemIndexed(wpc_, i, nullptr, 0);
			if (key_len <= 0) {
				continue;
			}
			char *key = (char *)malloc((size_t)key_len + 1);
			if (!key) {
				continue;
			}
			if (WavpackGetTagItemIndexed(wpc_, i, key, key_len + 1) <= 0) {
				free(key);
				continue;
			}

			int value_len = WavpackGetTagItem(wpc_, key, nullptr, 0);
			if (value_len <= 0) {
				free(key);
				continue;
			}
			char *value = (char *)malloc((size_t)value_len + 1);
			if (!value) {
				free(key);
				continue;
			}
			int copied = WavpackGetTagItem(wpc_, key, value, value_len + 1);
			if (copied <= 0) {
				free(value);
				free(key);
				continue;
			}

			espaudiocore_meta_t meta = {};
			meta.type = key;
			meta.data = value;
			meta.len = strlen(value);
			meta.pic_type = -1;
			meta_cb_(meta_user_, &meta);
			free(value);
			free(key);
		}

		const int binary_count = WavpackGetNumBinaryTagItems(wpc_);
		for (int i = 0; i < binary_count; ++i) {
			int key_len = WavpackGetBinaryTagItemIndexed(wpc_, i, nullptr, 0);
			if (key_len <= 0) {
				continue;
			}
			char *key = (char *)malloc((size_t)key_len + 1);
			if (!key) {
				continue;
			}
			if (WavpackGetBinaryTagItemIndexed(wpc_, i, key, key_len + 1) <= 0 ||
				!is_cover_art_tag(key)) {
				free(key);
				continue;
			}

			int data_len = WavpackGetBinaryTagItem(wpc_, key, nullptr, 0);
			if (data_len <= 0) {
				free(key);
				continue;
			}
			uint8_t *tag_data = (uint8_t *)malloc((size_t)data_len);
			if (!tag_data) {
				free(key);
				continue;
			}
			int copied = WavpackGetBinaryTagItem(wpc_, key, (char *)tag_data, data_len);
			int pic_type = cover_art_type(key);
			free(key);
			if (copied <= 0) {
				free(tag_data);
				continue;
			}

			uint8_t *image = (uint8_t *)memchr(tag_data, '\0', (size_t)copied);
			if (!image || image + 1 >= tag_data + copied) {
				free(tag_data);
				continue;
			}
			++image;

			espaudiocore_meta_t meta = {};
			meta.type = "APIC";
			meta.is_binary = true;
			meta.data = (const char *)image;
			meta.len = (size_t)(tag_data + copied - image);
			meta.mime = image_mime(image, meta.len);
			meta.pic_type = pic_type;
			meta_cb_(meta_user_, &meta);
			free(tag_data);
		}
	}

	static char ascii_lower(char value)
	{
		return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
	}

	static bool tag_name_equals(const char *tag, const char *name)
	{
		while (*tag && *name && ascii_lower(*tag) == ascii_lower(*name)) {
			++tag;
			++name;
		}
		return !*tag && !*name;
	}

	static bool is_cover_art_tag(const char *tag)
	{
		static const char prefix[] = "Cover Art (";
		for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
			if (!tag[i] || ascii_lower(tag[i]) != ascii_lower(prefix[i])) {
				return false;
			}
		}
		return true;
	}

	static int cover_art_type(const char *tag)
	{
		if (tag_name_equals(tag, "Cover Art (Front)")) {
			return 3;
		}
		if (tag_name_equals(tag, "Cover Art (Back)")) {
			return 4;
		}
		if (tag_name_equals(tag, "Cover Art (Media)")) {
			return 6;
		}
		return 0;
	}

	static const char *image_mime(const uint8_t *data, size_t len)
	{
		static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		if (len >= sizeof(png_signature) &&
			memcmp(data, png_signature, sizeof(png_signature)) == 0) {
			return "image/png";
		}
		if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
			return "image/jpeg";
		}
		if (len >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) {
			return "image/gif";
		}
		if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
			return "image/webp";
		}
		if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
			return "image/bmp";
		}
		return nullptr;
	}

	AudioInput *in_ = nullptr;
	AudioInput *correction_in_ = nullptr;
	AudioSink *out_ = nullptr;
	WavpackContext *wpc_ = nullptr;
	WavpackStreamReader64 reader_ = {};
	InputReaderContext main_reader_ = {};
	InputReaderContext correction_reader_ = {};
	int32_t *sample_buf_ = nullptr;
	int32_t *pcm32_ = nullptr;
	int16_t *pcm16_ = nullptr;
	espaudiocore_meta_cb_t meta_cb_ = nullptr;
	void *meta_user_ = nullptr;
	int channels_ = 0;
	int src_bits_ = 0;
	int64_t duration_hint_ms_ = -1;
};

bool probe_wavpack(AudioInput *in)
{
	uint8_t header[4];
	return in && in->peek(header, sizeof(header), sizeof(header)) == sizeof(header) &&
		   memcmp(header, "wvpk", sizeof(header)) == 0;
}

AudioDecoder *create_wavpack(void)
{
	return new (std::nothrow) DecoderWavPack();
}

#else

bool probe_wavpack(AudioInput *in)
{
	(void)in;
	return false;
}

AudioDecoder *create_wavpack(void)
{
	return nullptr;
}

#endif