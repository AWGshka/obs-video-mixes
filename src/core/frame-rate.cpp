#include "core/frame-rate.hpp"

#include <obs.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vmix {

namespace {

constexpr int64_t kDshowMatchOutput = -1; /* win-dshow FPS_MATCHING */
constexpr int64_t kDshowCustomRes = 1;    /* win-dshow ResType_Custom */

double report(FpsBasis *out, FpsBasis basis, double fps)
{
	if (out)
		*out = basis;
	return fps;
}

} // namespace

double obs_fps()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.fps_den)
		return 0.0;
	return (double)ovi.fps_num / (double)ovi.fps_den;
}

double source_fps(obs_source_t *source, FpsBasis *basis)
{
	const uint32_t flags = source ? obs_source_get_output_flags(source) : 0;
	if (!(flags & OBS_SOURCE_VIDEO)) /* missing, or its plugin is not loaded */
		return report(basis, FpsBasis::Unknown, 0.0);

	const double output = obs_fps();
	const char *id = obs_source_get_unversioned_id(source);
	OBSDataAutoRelease s = obs_source_get_settings(source);

	if (std::strcmp(id, "browser_source") == 0) {
		if (obs_data_get_bool(s, "fps_custom"))
			return report(basis, FpsBasis::SourceRate, (double)obs_data_get_int(s, "fps"));
		return report(basis, FpsBasis::EveryFrame, output);
	}
	if (!(flags & OBS_SOURCE_ASYNC))
		return report(basis, FpsBasis::EveryFrame, output);

	if (std::strcmp(id, "dshow_input") == 0 && obs_data_get_int(s, "res_type") == kDshowCustomRes) {
		const int64_t interval = obs_data_get_int(s, "frame_interval");
		if (interval == kDshowMatchOutput)
			return report(basis, FpsBasis::SourceRate, output);
		if (interval > 0)
			return report(basis, FpsBasis::SourceRate, 10000000.0 / (double)interval);
	}
	/* Async sources that don't expose a rate: device-default webcams, phone cameras, media files, capture cards. */
	return report(basis, FpsBasis::Unknown, 0.0);
}

FrameRateChoice resolve_frame_rate(const MixConfig &cfg, obs_source_t *source)
{
	const double output = obs_fps();
	if (cfg.fps_divisor >= 1)
		return {cfg.fps_divisor, output / cfg.fps_divisor, FpsBasis::Fixed};
	if (cfg.kind != MixKind::Source)
		return {1, output, FpsBasis::Output};

	FpsBasis basis = FpsBasis::Unknown;
	const double src = source_fps(source, &basis);
	if (src <= 0.0 || output <= 0.0)
		return {1, output, FpsBasis::Unknown};
	/* Largest divisor whose output still covers the source rate; tolerance so 59.94/29.97 and 60/30 give 2. */
	const uint32_t divisor = std::max(1u, (uint32_t)std::floor(output / src + 0.02));
	return {divisor, output / divisor, basis};
}

} // namespace vmix
