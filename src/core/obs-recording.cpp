#include "core/obs-recording.hpp"
#include "core/encoders.hpp"
#include "core/util.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <cstring>
#include <string>

namespace vmix {

namespace {

bool equals(const char *a, const char *b)
{
	return a && b && std::strcmp(a, b) == 0;
}

/* Frontend encoder object names (frontend/utility/SimpleOutput.cpp, AdvancedOutput.cpp). */
OBSEncoderAutoRelease frontend_video_encoder(config_t *profile, bool advanced)
{
	const char *recording = advanced ? "advanced_video_recording" : "simple_video_recording";
	const char *streaming = advanced ? "advanced_video_stream" : "simple_video_stream";

	const bool uses_stream_encoder =
		advanced ? equals(config_get_string(profile, "AdvOut", "RecEncoder"), "none")
			 : equals(config_get_string(profile, "SimpleOutput", "RecQuality"), "Stream");

	OBSEncoderAutoRelease enc = obs_get_encoder_by_name(uses_stream_encoder ? streaming : recording);
	if (!enc)
		enc = obs_get_encoder_by_name(streaming);
	return enc;
}

bool container_from_format(const char *fmt, Container &out)
{
	if (!fmt || !*fmt)
		return false;
	if (equals(fmt, "hybrid_mp4"))
		out = Container::HybridMp4;
	else if (equals(fmt, "hybrid_mov"))
		out = Container::HybridMov;
	else if (equals(fmt, "mp4") || equals(fmt, "fragmented_mp4"))
		out = Container::Mp4;
	else if (equals(fmt, "mov") || equals(fmt, "fragmented_mov"))
		out = Container::Mov;
	else
		out = Container::Mkv; /* mkv, and formats a mix does not offer (flv, ts, m3u8) */
	return true;
}

void inherit_audio(MixConfig &cfg, obs_output_t *rec, config_t *profile, bool advanced)
{
	/* Exact: audio encoders attached to the recording output. */
	if (rec) {
		uint32_t tracks = 0;
		int bitrate = 0;
		for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {
			obs_encoder_t *a = obs_output_get_audio_encoder(rec, i);
			if (!a)
				continue;
			tracks |= 1u << obs_encoder_get_mixer_index(a);
			if (!bitrate) {
				OBSDataAutoRelease s = obs_encoder_get_settings(a);
				bitrate = (int)obs_data_get_int(s, "bitrate");
			}
		}
		if (tracks) {
			cfg.audio_tracks = tracks;
			if (bitrate > 0)
				cfg.audio_bitrate = bitrate;
			return;
		}
	}

	if (!profile)
		return;
	const int tracks = (int)config_get_int(profile, advanced ? "AdvOut" : "SimpleOutput", "RecTracks");
	if (tracks > 0)
		cfg.audio_tracks = (uint32_t)tracks & ((1u << kAudioTracks) - 1);
	if (advanced) {
		for (int i = 0; i < kAudioTracks; i++) {
			if (!(cfg.audio_tracks & (1u << i)))
				continue;
			const std::string key = "Track" + std::to_string(i + 1) + "Bitrate";
			const int kbps = (int)config_get_uint(profile, "AdvOut", key.c_str());
			if (kbps > 0)
				cfg.audio_bitrate = kbps;
			break;
		}
	} else if (const int kbps = (int)config_get_uint(profile, "SimpleOutput", "ABitrate"); kbps > 0) {
		cfg.audio_bitrate = kbps;
	}
}

} // namespace

bool inherit_obs_recording(MixConfig &cfg)
{
	config_t *profile = obs_frontend_get_profile_config();
	const bool advanced = profile && equals(config_get_string(profile, "Output", "Mode"), "Advanced");
	OBSOutputAutoRelease rec = obs_frontend_get_recording_output();
	bool found = false;

	/* Video encoder: the one OBS records with. */
	obs_encoder_t *enc = rec ? obs_output_get_video_encoder(rec) : nullptr;
	OBSEncoderAutoRelease named;
	if (!enc && profile) {
		named = frontend_video_encoder(profile, advanced);
		enc = named;
	}
	if (enc && encoder_selectable(obs_encoder_get_id(enc))) {
		cfg.encoder_id = obs_encoder_get_id(enc);
		cfg.encoder_settings = adopt(obs_encoder_get_settings(enc));
		/* Simple mode applies its recording quality only when a recording starts; before that the encoder has
		 * no settings and would fall back to its CBR default. */
		if (!obs_data_has_user_value(cfg.encoder_settings, "rate_control") &&
		    !obs_data_has_user_value(cfg.encoder_settings, "bitrate"))
			apply_quality_defaults(cfg.encoder_id.c_str(), cfg.encoder_settings);
		found = true;
	}

	inherit_audio(cfg, rec, profile, advanced);

	if (profile && !(advanced && equals(config_get_string(profile, "AdvOut", "RecType"), "FFmpeg"))) {
		Container c;
		if (container_from_format(config_get_string(profile, advanced ? "AdvOut" : "SimpleOutput", "RecFormat2"), c)) {
			cfg.container = c;
			found = true;
		}
	}

	return found;
}

} // namespace vmix
