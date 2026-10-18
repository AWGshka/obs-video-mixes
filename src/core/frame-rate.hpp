#pragma once

#include "core/config.hpp"

#include <obs.h>

#include <cstdint>

namespace vmix {

enum class FpsBasis {
	Fixed,      /* chosen by the user */
	Output,     /* program mix: OBS fps */
	SourceRate, /* the source's configured frame rate */
	EveryFrame, /* source is redrawn every OBS frame (captures, browser, scenes) */
	Unknown,    /* source does not report a rate (or has no video): OBS fps */
};

struct FrameRateChoice {
	uint32_t divisor; /* output fps = OBS fps / divisor */
	double fps;       /* resulting output fps */
	FpsBasis basis;
};

/* OBS' configured video frame rate. */
double obs_fps();

/*
 * Frame rate a source delivers, or 0 if it does not say. libobs exposes no observer for async frames
 * (obs_source_get_frame consumes the frame), so this reads what the source is configured for:
 *   - Video Capture Device (dshow_input) with a custom rate: that rate; "match output" = OBS fps
 *   - Browser source: its custom fps, otherwise it renders every OBS frame
 *   - synchronous sources (game/window/display capture, scenes, images): rendered every OBS frame
 *   - other async sources (device-default webcams, phone cameras, media, capture cards): unknown
 */
double source_fps(obs_source_t *source, FpsBasis *basis = nullptr);

/*
 * Encoder divisor for a mix. Fixed divisors are used as-is. Auto picks the largest divisor whose output rate still
 * covers the source's own frame rate (never drops unique frames), e.g. OBS 60 + webcam 30 → 2, webcam 25 → 2.
 */
FrameRateChoice resolve_frame_rate(const MixConfig &cfg, obs_source_t *source);

} // namespace vmix
