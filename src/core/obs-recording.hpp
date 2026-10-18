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

} // namespace vmix
