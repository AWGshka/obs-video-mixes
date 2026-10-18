#pragma once

#include "core/config.hpp"

#include <obs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vmix {

/* Called on the UI thread when a recording's output stopped (code is an OBS_OUTPUT_* value). */
using RecordingStoppedFn = void (*)(uint64_t token, int code, const std::string &error);
void set_recording_stopped_handler(RecordingStoppedFn fn);

/* One file recording of a mix canvas: video encoder on the canvas video, AAC per selected track, muxer. */
class MixRecording {
public:
	/* fps_divisor: encode every Nth canvas frame (output fps = OBS fps / N). */
	static std::unique_ptr<MixRecording> start(const MixConfig &cfg, video_t *video, uint32_t fps_divisor,
						   const std::string &path, uint64_t token, std::string &error);
	~MixRecording();

	MixRecording(const MixRecording &) = delete;
	MixRecording &operator=(const MixRecording &) = delete;

	void stop();
	bool pause(bool paused);
	bool paused() const { return obs_output_paused(output_); }

	const std::string &path() const { return path_; }
	uint64_t started_ns() const { return started_ns_; }
	int dropped_frames() const { return obs_output_get_frames_dropped(output_); }
	int total_frames() const { return obs_output_get_total_frames(output_); }

private:
	MixRecording() = default;
	static void on_stop(void *param, calldata_t *cd);

	/* Declaration order = reverse destruction order: signal, then output, then its encoders. */
	OBSEncoder video_encoder_;
	std::vector<OBSEncoder> audio_encoders_;
	OBSOutput output_;
	OBSSignal stop_signal_;
	std::string path_;
	uint64_t token_ = 0;
	uint64_t started_ns_ = 0;
};

std::string output_error_message(int code, const std::string &detail);

} // namespace vmix
