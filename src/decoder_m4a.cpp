/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_m4a.cpp
 * @brief   M4A/MP4 AAC-LC 解码器（avpack + libhelix-aac）。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"

#include <limits.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_M4A) && defined(CONFIG_ESPAUDIOCORE_ENABLE_AAC)


extern "C" {
#include "aacdec.h"
#include <avpack/mp4-read.h>
#include <avpack/mmtag.h>
#include <ffbase/string.h>
}

#define M4A_INPUT_SIZE 8192
#define M4A_PCM_SAMPLES (1024 * 2 * 2)

class DecoderM4A : public AudioDecoder {
public:
	~DecoderM4A() override
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
		if (!in_->can_seek() || in_->size() < 0) {
			set_decode_error(dec_err, AUDIO_DEC_ERR_OPEN);
			return AUDIO_ERR_NOT_SUPPORTED;
		}

		read_buf_ = (char *)audio_alloc_big(M4A_INPUT_SIZE * 2, 0);
		if (!read_buf_) {
			set_decode_error(dec_err, AUDIO_DEC_ERR_OPEN);
			close();
			return AUDIO_ERR_NO_MEM;
		}

		mp4_ = {};
		input_ = {};
		input_eof_ = false;
		read_buf_index_ = 1;
		mp4read_open(&mp4_);
		mp4_initialized_ = true;
		mp4_.total_size = (ffuint64)in_->size();
		while (!configured_) {
			ffstr sample = {};
			int result = next_sample(&sample);
			if (result == 2) {
				break;
			}
			if (result < 0) {
				set_decode_error(dec_err, AUDIO_DEC_ERR_PARSE);
				close();
				return (audio_err_t)(-result);
			}
			if (result == 0) {
				set_decode_error(dec_err, AUDIO_DEC_ERR_PARSE);
				close();
				return AUDIO_ERR_PARSE;
			}
		}

		if (fmt) {
			*fmt = fmt_;
		}
		out_->set_rate(fmt_.rate);
		out_->set_format(fmt_.bits, fmt_.channels);
		if (duration_ms) {
			*duration_ms = duration_ms_;
		}
		state_ = AUDIO_DEC_STATE_ACTIVE;
		AUDIO_LOGI("M4A: AAC-LC %u Hz, %u ch, duration=%lld ms",
				   (unsigned)fmt_.rate, (unsigned)fmt_.channels,
				   (long long)duration_ms_);
		return AUDIO_OK;
	}

	audio_err_t decode() override
	{
		if (state_ != AUDIO_DEC_STATE_ACTIVE || !aac_) {
			return AUDIO_ERR_EOF;
		}

		ffstr sample = {};
		int result = next_sample(&sample);
		if (result == 0) {
			state_ = AUDIO_DEC_STATE_EOS;
			return AUDIO_ERR_EOF;
		}
		if (result < 0) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = (audio_err_t)(-result);
			return (audio_err_t)(-result);
		}
		if (sample.len == 0 || sample.len > INT_MAX) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = AUDIO_ERR_DECODE;
			return AUDIO_ERR_DECODE;
		}

		unsigned char *sample_data = (unsigned char *)sample.ptr;
		int bytes_left = (int)sample.len;
		int rc = AACDecode(aac_, &sample_data, &bytes_left, pcm16_);
		if (rc != ERR_AAC_NONE) {
			AUDIO_LOGW("M4A AAC frame decode failed: %d", rc);
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = AUDIO_ERR_DECODE;
			return AUDIO_ERR_DECODE;
		}

		AACFrameInfo frame = {};
		AACGetLastFrameInfo(aac_, &frame);
		if (frame.sampRateOut <= 0 || frame.nChans <= 0 || frame.nChans > 2 ||
			frame.outputSamps <= 0 || frame.outputSamps % frame.nChans != 0) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = AUDIO_ERR_DECODE;
			return AUDIO_ERR_DECODE;
		}
		const size_t frames = (size_t)frame.outputSamps / (size_t)frame.nChans;
		if (frames > M4A_PCM_SAMPLES / 2) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = AUDIO_ERR_DECODE;
			return AUDIO_ERR_DECODE;
		}

		if (fmt_.rate != (uint32_t)frame.sampRateOut) {
			fmt_.rate = (uint32_t)frame.sampRateOut;
			out_->set_rate(fmt_.rate);
		}
		for (size_t i = 0; i < frames; ++i) {
			if (frame.nChans == 1) {
				int32_t value = (int32_t)pcm16_[i] << 16;
				pcm32_[i * 2] = value;
				pcm32_[i * 2 + 1] = value;
			} else {
				pcm32_[i * 2] = (int32_t)pcm16_[i * 2] << 16;
				pcm32_[i * 2 + 1] = (int32_t)pcm16_[i * 2 + 1] << 16;
			}
		}

		rc = out_->write_i32(pcm32_, frames, 16, 0);
		if (rc == AUDIO_ERR_NOT_SUPPORTED) {
			if (frame.nChans == 1) {
				for (size_t i = 0; i < frames; ++i) {
					pcm16_[i * 2] = pcm16_[i * 2 + 1] =
						(int16_t)(pcm32_[i * 2] >> 16);
				}
			}
			rc = out_->write(pcm16_, frames * 2 * sizeof(int16_t), 0);
		}
		if (rc != AUDIO_OK) {
			state_ = AUDIO_DEC_STATE_ERROR;
			last_error_ = (audio_err_t)rc;
			return (audio_err_t)rc;
		}
		return AUDIO_OK;
	}

	audio_err_t seek_ms(int64_t ms) override
	{
		if (!configured_ || !in_ || !in_->can_seek()) {
			return AUDIO_ERR_NOT_SUPPORTED;
		}
		if (ms < 0) {
			ms = 0;
		}
		ffuint64 sample;
		if (total_samples_ && duration_ms_ >= 0 && ms >= duration_ms_) {
			sample = total_samples_ - 1;
		} else {
			ffuint64 seconds = (ffuint64)ms / 1000;
			ffuint64 remainder = (ffuint64)ms % 1000;
			sample = seconds * container_rate_ + remainder * container_rate_ / 1000;
			if (total_samples_ && sample >= total_samples_) {
				sample = total_samples_ - 1;
			}
		}
		mp4read_seek(&mp4_, sample);
		input_eof_ = false;
		(void)AACFlushCodec(aac_);
		state_ = AUDIO_DEC_STATE_ACTIVE;
		return AUDIO_OK;
	}

	void reset() override
	{
		if (aac_) {
			(void)AACFlushCodec(aac_);
		}
		state_ = AUDIO_DEC_STATE_ACTIVE;
	}

	void close() override
	{
		if (mp4_initialized_) {
			mp4read_close(&mp4_);
			mp4_initialized_ = false;
		}
		if (aac_) {
			AACFreeDecoder(aac_);
			aac_ = nullptr;
		}
		audio_free(read_buf_);
		read_buf_ = nullptr;
		audio_free(pcm16_);
		pcm16_ = nullptr;
		audio_free(pcm32_);
		pcm32_ = nullptr;
		input_ = {};
		in_ = nullptr;
		out_ = nullptr;
		meta_cb_ = nullptr;
		meta_user_ = nullptr;
		configured_ = false;
		input_eof_ = false;
		read_buf_index_ = 1;
		total_samples_ = 0;
		duration_ms_ = -1;
		container_rate_ = 0;
		state_ = AUDIO_DEC_STATE_IDLE;
	}

private:
	struct BitReader {
		const uint8_t *data;
		size_t bits;
		size_t pos = 0;

		bool read(unsigned count, uint32_t *value)
		{
			if (count > 32 || pos + count > bits) {
				return false;
			}
			uint32_t result = 0;
			for (unsigned i = 0; i < count; ++i, ++pos) {
				result = (result << 1) | ((data[pos / 8] >> (7 - (pos % 8))) & 1u);
			}
			*value = result;
			return true;
		}
	};

	static void set_decode_error(int *dec_err, int error)
	{
		if (dec_err) {
			*dec_err = error;
		}
	}

	static bool read_audio_specific_config(ffstr config, int *sample_rate, int *channels)
	{
		static const uint32_t sample_rates[] = {
			96000, 88200, 64000, 48000, 44100, 32000, 24000,
			22050, 16000, 12000, 11025, 8000, 7350
		};
		if (!config.ptr || config.len < 2) {
			return false;
		}
		BitReader bits = {(const uint8_t *)config.ptr, config.len * 8};
		uint32_t object_type, rate_index, channel_config, rate;
		if (!bits.read(5, &object_type)) {
			return false;
		}
		if (object_type == 31) {
			uint32_t extension;
			if (!bits.read(6, &extension)) {
				return false;
			}
			object_type = 32 + extension;
		}
		if (!bits.read(4, &rate_index)) {
			return false;
		}
		if (rate_index == 15) {
			if (!bits.read(24, &rate)) {
				return false;
			}
		} else if (rate_index < sizeof(sample_rates) / sizeof(sample_rates[0])) {
			rate = sample_rates[rate_index];
		} else {
			return false;
		}
		if (!bits.read(4, &channel_config) ||
			(channel_config != 1 && channel_config != 2) || rate == 0 || rate > INT_MAX) {
			return false;
		}
		if (object_type == 5) {
			uint32_t extension_index, extension_type, extension_rate;
			if (!bits.read(4, &extension_index)) {
				return false;
			}
			if (extension_index == 15) {
				if (!bits.read(24, &extension_rate)) {
					return false;
				}
			} else if (extension_index < sizeof(sample_rates) / sizeof(sample_rates[0])) {
				extension_rate = sample_rates[extension_index];
			} else {
				return false;
			}
			if (!bits.read(5, &extension_type) || extension_type != 2 || extension_rate == 0) {
				return false;
			}
			rate = extension_rate;
		} else if (object_type != 2) {
			return false;
		}
		*sample_rate = (int)rate;
		*channels = (int)channel_config;
		return true;
	}

	bool configure_track()
	{
		const mp4read_audio_info *audio = nullptr;
		for (int i = 0;; ++i) {
			audio = (const mp4read_audio_info *)mp4read_track_info(&mp4_, i);
			if (!audio) {
				return false;
			}
			if (audio->type == 1) {
				if (audio->codec != AVPKC_AAC) {
					AUDIO_LOGE("M4A audio codec is not AAC");
					return false;
				}
				mp4read_track_activate(&mp4_, i);
				break;
			}
		}

		int sample_rate = 0;
		int channels = 0;
		if (!read_audio_specific_config(audio->codec_conf, &sample_rate, &channels)) {
			AUDIO_LOGE("M4A AudioSpecificConfig is unsupported (AAC-LC mono/stereo required)");
			return false;
		}
		aac_ = AACInitDecoder();
		pcm16_ = (int16_t *)audio_alloc_big(M4A_PCM_SAMPLES * sizeof(int16_t), 0);
		pcm32_ = (int32_t *)audio_alloc_big(M4A_PCM_SAMPLES * sizeof(int32_t), 0);
		if (!aac_ || !pcm16_ || !pcm32_) {
			return false;
		}
		AACFrameInfo params = {};
		params.nChans = channels;
		params.sampRateCore = sample_rate;
		params.profile = AAC_PROFILE_LC;
		if (AACSetRawBlockParams(aac_, 0, &params) != ERR_AAC_NONE) {
			AUDIO_LOGE("Helix rejected M4A AAC raw-block parameters");
			return false;
		}

		container_rate_ = (audio->format.rate != 0) ? audio->format.rate : (uint32_t)sample_rate;
		fmt_.rate = container_rate_;
		fmt_.channels = 2;
		fmt_.bits = 16;
		total_samples_ = audio->total_samples;
		if (total_samples_ != 0 && container_rate_ != 0) {
			duration_ms_ = (int64_t)(total_samples_ * 1000ull / container_rate_);
		}
		configured_ = true;
        AUDIO_LOGI("M4A track: rate=%u container_rate=%u total_samples=%llu duration=%lld ms",
           (unsigned)audio->format.rate, (unsigned)container_rate_,
           (unsigned long long)total_samples_, (long long)duration_ms_);
		return true;
	}

	static const char *tag_name(unsigned tag)
	{
		switch (tag) {
		case MMTAG_ALBUM: return "Album";
		case MMTAG_ALBUMARTIST: return "AlbumArtist";
		case MMTAG_ARTIST: return "Artist";
		case MMTAG_COMMENT: return "Comment";
		case MMTAG_COMPOSER: return "Composer";
		case MMTAG_COPYRIGHT: return "Copyright";
		case MMTAG_DATE: return "Date";
		case MMTAG_DISCNUMBER: return "DiscNumber";
		case MMTAG_GENRE: return "Genre";
		case MMTAG_LYRICS: return "Lyrics";
		case MMTAG_PUBLISHER: return "Publisher";
		case MMTAG_TITLE: return "Title";
		case MMTAG_TRACKNO: return "Track";
		case MMTAG_TRACKTOTAL: return "TrackTotal";
		default: return "Metadata";
		}
	}

	void report_tag()
	{
		if (!meta_cb_) {
			return;
		}
		ffstr value = {};
		unsigned tag = (unsigned)mp4read_tag(&mp4_, &value);
		if (tag == MMTAG_PICTURE) {
			if (value.len == 0) {
				return;
			}
			avpk_pic pic = {};
			ffstr image = {};
			const char *mime = nullptr;
			if (value.len >= 8 && avpk_pic_read(value, &pic, &image)) {
				mime = pic.mime;
			} else {
				image = value;
				mime = image_mime((const uint8_t *)image.ptr, image.len);
			}
			espaudiocore_meta_t meta = {};
			meta.type = "PICTURE";
			meta.is_binary = true;
			meta.data = image.ptr;
			meta.len = image.len;
			meta.mime = mime;
			meta.pic_type = 3;
			meta_cb_(meta_user_, &meta);
			return;
		}
		if (value.len == 0 || value.len == SIZE_MAX) {
			return;
		}
		char *text = (char *)malloc(value.len + 1);
		if (!text) {
			return;
		}
		memcpy(text, value.ptr, value.len);
		text[value.len] = '\0';
		espaudiocore_meta_t meta = {};
		meta.type = tag_name(tag);
		meta.data = text;
		meta.len = strlen(text);
		meta_cb_(meta_user_, &meta);
		free(text);
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
		return nullptr;
	}

	int refill_input()
	{
		size_t remaining = input_.len;
		if (remaining >= M4A_INPUT_SIZE) {
			return -1;
		}
		read_buf_index_ ^= 1;
		char *buffer = read_buf_ + read_buf_index_ * M4A_INPUT_SIZE;
		if (remaining && input_.ptr != buffer) {
			memmove(buffer, input_.ptr, remaining);
		}
		int got = in_->read(buffer + remaining, M4A_INPUT_SIZE - remaining);
		if (got < 0) {
			return -1;
		}
		ffstr_set(&input_, buffer, remaining + (size_t)got);
		return got;
	}

	int next_sample(ffstr *sample)
	{
		for (;;) {
			if (input_.len == 0 && !input_eof_) {
				int got = refill_input();
				if (got < 0) {
					return -AUDIO_ERR_IO;
				}
				input_eof_ = (got == 0);
			}
			int result = mp4read_process(&mp4_, &input_, sample);
			switch (result) {
			case MP4READ_HEADER:
				if (!configure_track()) {
					return -AUDIO_ERR_NOT_SUPPORTED;
				}
				return 2;
			case MP4READ_TAG:
				report_tag();
				break;
			case MP4READ_DATA:
				return 1;
			case MP4READ_SEEK: {
				ffuint64 offset = mp4read_offset(&mp4_);
				if (offset > INT64_MAX || in_->seek((int64_t)offset, SEEK_SET) != AUDIO_OK) {
					return -AUDIO_ERR_IO;
				}
				ffstr_null(&input_);
				input_eof_ = false;
				break;
			}
			case MP4READ_MORE:
				if (input_eof_) {
					return 0;
				}
				{
					int got = refill_input();
					if (got < 0) {
						return -AUDIO_ERR_IO;
					}
					input_eof_ = (got == 0);
				}
				break;
			case MP4READ_DONE:
				return 0;
			case MP4READ_ERROR:
			default:
				AUDIO_LOGE("M4A parse failed at %lld: %s", (long long)mp4_.off,
						   mp4read_error(&mp4_));
				return -AUDIO_ERR_PARSE;
			}
		}
	}

	AudioInput *in_ = nullptr;
	AudioSink *out_ = nullptr;
	HAACDecoder aac_ = nullptr;
	mp4read mp4_ = {};
	ffstr input_ = {};
	char *read_buf_ = nullptr;
	unsigned read_buf_index_ = 1;
	int16_t *pcm16_ = nullptr;
	int32_t *pcm32_ = nullptr;
	espaudiocore_meta_cb_t meta_cb_ = nullptr;
	void *meta_user_ = nullptr;
	ffuint64 total_samples_ = 0;
	uint32_t container_rate_ = 0;
	int64_t duration_ms_ = -1;
	bool mp4_initialized_ = false;
	bool configured_ = false;
	bool input_eof_ = false;
};

bool probe_m4a(AudioInput *in)
{
	uint8_t header[8];
	return in && in->peek(header, sizeof(header), sizeof(header)) == sizeof(header) &&
		   memcmp(header + 4, "ftyp", 4) == 0;
}

AudioDecoder *create_m4a(void)
{
	return new (std::nothrow) DecoderM4A();
}

#else

bool probe_m4a(AudioInput *in)
{
	(void)in;
	return false;
}

AudioDecoder *create_m4a(void)
{
	return nullptr;
}

#endif