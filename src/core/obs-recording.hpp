#pragma once

#include "core/config.hpp"

namespace vmix {

/*
 * Copies OBS' own recording setup into a mix: video encoder + all its settings (rate control, bitrate, preset...),
 * audio tracks and bitrate, and the file format. Works for Simple and Advanced output mode.
 *
 * Sources, most exact first: the encoders attached to the frontend's recording output (after a recording ran this
 * session), then the frontend's encoder objects, then the profile config. Returns false if nothing usable was found;
 * fields that could not be determined keep their current values.
 */
bool inherit_obs_recording(MixConfig &cfg);

/*
 * Copies OBS' streaming encoder (with its bitrate and rate control) and stream audio track/bitrate into the mix's stream
 * settings. The server and key are not copied: a mix streams somewhere else. Returns false if nothing usable was found.
 */
bool inherit_obs_stream(MixConfig &cfg);

} // namespace vmix
