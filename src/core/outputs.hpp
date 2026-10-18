#pragma once

#include "core/config.hpp"
#include "core/source-audio.hpp"

#include <obs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vmix {

enum class OutputKind : int { Record = 0, Replay = 1, Stream = 2 };
constexpr int kOutputKinds = 3;

/* What a mix encodes for one purpose (files, or a stream). */
struct EncoderSpec {
	std::string name;          /* object name prefix */
	std::string encoder_id;
	OBSData encoder_settings;
	uint32_t audio_tracks = 1; /* bit n = mixer track n+1; ignored with source_audio */
	int audio_bitrate = 192;
	obs_source_t *source_audio = nullptr; /* encode this source's own audio instead of mixer tracks */
};

/*
 * Video + audio encoders on a mix canvas, shared by the outputs that use them (recording and replay buffer encode
 * once). The encoders stop when the last output using them stops; the set is freed after its outputs.
 */
class EncoderSet {
public:
	static std::shared_ptr<EncoderSet> create(const EncoderSpec &spec, video_t *video, uint32_t fps_divisor,
						  std::string &error);
	~EncoderSet();

	obs_encoder_t *video() const { return video_; }
	const std::vector<OBSEncoder> &audio() const { return audio_; }

private:
	EncoderSet() = default;

	std::unique_ptr<SourceAudio> source_audio_; /* outlives the audio encoders connected to it */
	OBSEncoder video_;
	std::vector<OBSEncoder> audio_;
};

struct OutputSpec {
	std::string name; /* object name */

	/* Record */
	std::string path;
	Container container = Container::Mkv;

	/* Replay */
	std::string directory;
	std::string filename_format; /* os_generate_formatted_filename format */
	std::string extension;
	int max_seconds = 30;
	int max_mb = 512;

	/* Stream */
	std::string server;
	std::string key;
};

enum class OutputEvent { Stopped, ReplaySaved, Reconnecting, Reconnected };

/* Called on the UI thread for output events (code: an OBS_OUTPUT_* value for Stopped). */
using OutputEventFn = void (*)(uint64_t token, OutputEvent event, int code, const std::string &text);
void set_output_event_handler(OutputEventFn fn);

struct SignalContext;

/* One running output of a mix: a file recording, a replay buffer or a stream. */
class MixOutput {
public:
	static std::unique_ptr<MixOutput> start(OutputKind kind, const OutputSpec &spec,
						std::shared_ptr<EncoderSet> encoders, uint64_t token, std::string &error);
	~MixOutput();

	MixOutput(const MixOutput &) = delete;
	MixOutput &operator=(const MixOutput &) = delete;

	OutputKind kind() const { return kind_; }
	void stop();
	bool pause(bool paused);
	bool paused() const { return obs_output_paused(output_); }
	bool save_replay();
	std::string last_replay() const;
	bool reconnecting() const { return obs_output_reconnecting(output_); }

	const std::string &path() const { return path_; }
	uint64_t started_ns() const { return started_ns_; }
	int dropped_frames() const { return obs_output_get_frames_dropped(output_); }
	int total_frames() const { return obs_output_get_total_frames(output_); }
	uint64_t total_bytes() const { return obs_output_get_total_bytes(output_); }
	float congestion() const { return obs_output_get_congestion(output_); }

private:
	MixOutput() = default;

	OutputKind kind_ = OutputKind::Record;
	/* Declaration order = reverse destruction order: signals, output, service, then the encoders. */
	std::shared_ptr<EncoderSet> encoders_;
	OBSService service_;
	OBSOutput output_;
	std::unique_ptr<SignalContext> context_; /* signal parameter; outlives the connections */
	std::vector<OBSSignal> signals_;
	std::string path_;
	uint64_t started_ns_ = 0;
};

std::string output_error_message(int code, const std::string &detail);

} // namespace vmix
