#include "core/recording.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace vmix {

namespace {

RecordingStoppedFn g_stopped = nullptr;

struct StopEvent {
	uint64_t token;
	int code;
	std::string error;
};

const char *audio_encoder_id()
{
	static constexpr std::array kIds = {"ffmpeg_aac", "CoreAudio_AAC", "libfdk_aac"};
	const char *id = nullptr;
	for (const char *want : kIds)
		for (size_t i = 0; obs_enum_encoder_types(i, &id); i++)
			if (std::strcmp(id, want) == 0)
				return want;
	return "ffmpeg_aac";
}

} // namespace

void set_recording_stopped_handler(RecordingStoppedFn fn)
{
	g_stopped = fn;
}

std::unique_ptr<MixRecording> MixRecording::start(const MixConfig &cfg, video_t *video, uint32_t fps_divisor,
						  const std::string &path, uint64_t token, std::string &error)
{
	if (!video) {
		error = obs_module_text("Error.NoVideo");
		return nullptr;
	}

	std::unique_ptr<MixRecording> rec(new MixRecording());
	rec->path_ = path;
	rec->token_ = token;

	const std::string base = "video-mixes-" + std::to_string(cfg.id);

	OBSEncoderAutoRelease venc = obs_video_encoder_create(cfg.encoder_id.c_str(), (base + "-video").c_str(),
							      cfg.encoder_settings, nullptr);
	if (!venc) {
		error = std::string(obs_module_text("Error.Encoder")) + " (" + cfg.encoder_id + ")";
		return nullptr;
	}
	obs_encoder_set_video(venc, video);
	if (fps_divisor > 1)
		obs_encoder_set_frame_rate_divisor(venc, fps_divisor);
	rec->video_encoder_ = venc.Get();

	const char *output_id = cfg.container == Container::HybridMp4   ? "mp4_output"
				: cfg.container == Container::HybridMov ? "mov_output"
									: "ffmpeg_muxer";
	OBSDataAutoRelease out_settings = obs_data_create();
	obs_data_set_string(out_settings, "path", path.c_str());
	OBSOutputAutoRelease output = obs_output_create(output_id, (base + "-output").c_str(), out_settings, nullptr);
	if (!output) {
		error = obs_module_text("Error.Output");
		return nullptr;
	}
	rec->output_ = output.Get();
	obs_output_set_video_encoder(output, venc);

	const char *aac = audio_encoder_id();
	size_t audio_idx = 0;
	for (int track = 0; track < kAudioTracks; track++) {
		if (!(cfg.audio_tracks & (1u << track)))
			continue;
		OBSDataAutoRelease a_settings = obs_data_create();
		obs_data_set_int(a_settings, "bitrate", cfg.audio_bitrate);
		const std::string name = base + "-audio-" + std::to_string(track + 1);
		OBSEncoderAutoRelease aenc = obs_audio_encoder_create(aac, name.c_str(), a_settings, (size_t)track, nullptr);
		if (!aenc)
			continue;
		obs_encoder_set_audio(aenc, obs_get_audio());
		obs_output_set_audio_encoder(output, aenc, audio_idx++);
		rec->audio_encoders_.emplace_back(aenc.Get());
	}

	rec->stop_signal_.Connect(obs_output_get_signal_handler(output), "stop", &MixRecording::on_stop,
				  reinterpret_cast<void *>(static_cast<uintptr_t>(token)));

	if (!obs_output_start(output)) {
		const char *detail = obs_output_get_last_error(output);
		error = std::string(obs_module_text("Error.Start")) + (detail && *detail ? std::string(": ") + detail : "");
		rec->stop_signal_.Disconnect();
		return nullptr;
	}

	rec->started_ns_ = os_gettime_ns();
	blog(LOG_INFO, "[video-mixes] Recording mix '%s' → %s (%s, %ux%u, fps divisor %u)", cfg.name.c_str(),
	     path.c_str(), cfg.encoder_id.c_str(), obs_encoder_get_width(venc), obs_encoder_get_height(venc),
	     std::max(1u, fps_divisor));
	return rec;
}

MixRecording::~MixRecording()
{
	stop_signal_.Disconnect();
	if (output_ && obs_output_active(output_))
		obs_output_force_stop(output_);
}

void MixRecording::stop()
{
	obs_output_stop(output_);
}

bool MixRecording::pause(bool paused)
{
	return obs_output_pause(output_, paused);
}

/* Output thread: copy what is needed and hop to the UI thread. */
void MixRecording::on_stop(void *param, calldata_t *cd)
{
	auto *ev = new StopEvent{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(param)),
				 (int)calldata_int(cd, "code"), {}};
	if (obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"))) {
		const char *err = obs_output_get_last_error(out);
		ev->error = err ? err : "";
	}
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			std::unique_ptr<StopEvent> e(static_cast<StopEvent *>(p));
			if (g_stopped)
				g_stopped(e->token, e->code, e->error);
		},
		ev, false);
}

std::string output_error_message(int code, const std::string &detail)
{
	const char *key = "Error.Unknown";
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return {};
	case OBS_OUTPUT_BAD_PATH:
		key = "Error.BadPath";
		break;
	case OBS_OUTPUT_NO_SPACE:
		key = "Error.NoSpace";
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		key = "Error.EncodeError";
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		key = "Error.Unsupported";
		break;
	default:
		break;
	}
	std::string msg = obs_module_text(key);
	if (!detail.empty())
		msg += ": " + detail;
	return msg;
}

} // namespace vmix
