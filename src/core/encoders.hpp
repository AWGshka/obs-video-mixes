#pragma once

#include <obs.h>

#include <string>
#include <vector>

namespace vmix {

struct EncoderType {
	std::string id;
	std::string name;
	bool hardware; /* texture-based (GPU) encoder */
};

/*
 * Selectable video encoders (not deprecated/internal), best first: GPU texture encoders, then H.264 before other
 * codecs, then software. Selection is by capabilities and codec, never by hardcoded ids, so encoder renames in OBS
 * (e.g. jim_nvenc → obs_nvenc_h264_tex in OBS 31) cannot silently push mixes onto the CPU.
 */
std::vector<EncoderType> video_encoders();
bool encoder_selectable(const char *id);
std::string preferred_video_encoder();

/* Fresh settings for a local recording: the encoder's constant-quality mode, if it has one (+1 reference). */
obs_data_t *recording_defaults(const char *encoder_id);

/*
 * Switches settings to the encoder's constant-quality rate control (CQP / CRF / CQVBR ...) with a sensible quality
 * level, read from the encoder's own property list. Returns false if the encoder exposes no such mode.
 */
bool apply_quality_defaults(const char *encoder_id, obs_data_t *settings);

/* True for fixed-bitrate rate control, which wastes bits on simple frames and starves complex ones. */
bool uses_constant_bitrate(const char *encoder_id, obs_data_t *settings);

/* True if both hold the same user-set values (order-independent). */
bool same_settings(obs_data_t *a, obs_data_t *b);

} // namespace vmix
