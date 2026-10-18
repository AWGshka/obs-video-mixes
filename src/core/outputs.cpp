#include "core/outputs.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace vmix {

namespace {

OutputEventFn g_handler = nullptr;

struct EventMsg {
	uint64_t token;
	OutputEvent event;
	int code;
	std::string text;
};

/* Any thread → UI thread. */
void post(uint64_t token, OutputEvent event, int code, std::string text)
{
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			std::unique_ptr<EventMsg> m(static_cast<EventMsg *>(p));
			if (g_handler)
				g_handler(m->token, m->event, m->code, m->text);
		},
		new EventMsg{token, event, code, std::move(text)}, false);
}

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

OBSEncoderAutoRelease make_audio_encoder(const std::string &name, int bitrate, size_t mixer, audio_t *audio)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", bitrate);
	OBSEncoderAutoRelease enc = obs_audio_encoder_create(audio_encoder_id(), name.c_str(), settings, mixer, nullptr);
	if (enc)
		obs_encoder_set_audio(enc, audio);
	return enc;
}

/* Stream output type for a service, as the frontend picks it (BasicOutputHandler.cpp GetStreamOutputType). */
const char *stream_output_type(obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	if (!protocol || !obs_is_output_protocol_registered(protocol))
		return nullptr;
	const char *preferred = obs_service_get_preferred_output_type(service);
	if (preferred && (obs_get_output_flags(preferred) & OBS_OUTPUT_SERVICE))
		return preferred;
	if (std::strncmp(protocol, "RTMP", 4) == 0)
		return "rtmp_output";
	if (std::strcmp(protocol, "HLS") == 0)
		return "ffmpeg_hls_muxer";
	if (std::strcmp(protocol, "SRT") == 0 || std::strcmp(protocol, "RIST") == 0)
		return "ffmpeg_mpegts_muxer";
	return nullptr;
}

} // namespace

void set_output_event_handler(OutputEventFn fn)
{
	g_handler = fn;
}

/* ---- EncoderSet ---- */

std::shared_ptr<EncoderSet> EncoderSet::create(const EncoderSpec &spec, video_t *video, uint32_t fps_divisor,
					       std::string &error)
{
	if (!video) {
		error = obs_module_text("Error.NoVideo");
		return nullptr;
	}

	std::shared_ptr<EncoderSet> set(new EncoderSet());
	OBSEncoderAutoRelease venc = obs_video_encoder_create(spec.encoder_id.c_str(), (spec.name + "-video").c_str(),
							      spec.encoder_settings, nullptr);
	if (!venc) {
		error = std::string(obs_module_text("Error.Encoder")) + " (" + spec.encoder_id + ")";
		return nullptr;
	}
	obs_encoder_set_video(venc, video);
	if (fps_divisor > 1)
		obs_encoder_set_frame_rate_divisor(venc, fps_divisor);
	set->video_ = venc.Get();

	if (spec.source_audio) {
		set->source_audio_ = SourceAudio::create(spec.source_audio, error);
		if (!set->source_audio_)
			return nullptr;
		OBSEncoderAutoRelease aenc =
			make_audio_encoder(spec.name + "-audio", spec.audio_bitrate, 0, set->source_audio_->audio());
		if (aenc)
			set->audio_.emplace_back(aenc.Get());
	} else {
		for (int track = 0; track < kAudioTracks; track++) {
			if (!(spec.audio_tracks & (1u << track)))
				continue;
			OBSEncoderAutoRelease aenc =
				make_audio_encoder(spec.name + "-audio-" + std::to_string(track + 1), spec.audio_bitrate,
						   (size_t)track, obs_get_audio());
			if (aenc)
				set->audio_.emplace_back(aenc.Get());
		}
	}
	if (set->audio_.empty()) {
		error = obs_module_text("Error.AudioEncoder");
		return nullptr;
	}
	return set;
}

EncoderSet::~EncoderSet()
{
	/* Encoders disconnect from the source audio when released; release them before closing it. */
	audio_.clear();
	video_ = nullptr;
	source_audio_.reset();
}

/* ---- MixOutput ---- */

struct SignalContext {
	uint64_t token;
	obs_output_t *output;
};

std::unique_ptr<MixOutput> MixOutput::start(OutputKind kind, const OutputSpec &spec,
					    std::shared_ptr<EncoderSet> encoders, uint64_t token, std::string &error)
{
	std::unique_ptr<MixOutput> out(new MixOutput());
	out->kind_ = kind;
	out->encoders_ = std::move(encoders);

	OBSDataAutoRelease settings = obs_data_create();
	const char *output_id = "ffmpeg_muxer";
	switch (kind) {
	case OutputKind::Record:
		output_id = spec.container == Container::HybridMp4   ? "mp4_output"
			    : spec.container == Container::HybridMov ? "mov_output"
								     : "ffmpeg_muxer";
		obs_data_set_string(settings, "path", spec.path.c_str());
		out->path_ = spec.path;
		break;
	case OutputKind::Replay:
		output_id = "replay_buffer";
		obs_data_set_string(settings, "directory", spec.directory.c_str());
		obs_data_set_string(settings, "format", spec.filename_format.c_str());
		obs_data_set_string(settings, "extension", spec.extension.c_str());
		obs_data_set_bool(settings, "allow_spaces", true);
		obs_data_set_int(settings, "max_time_sec", spec.max_seconds);
		obs_data_set_int(settings, "max_size_mb", spec.max_mb);
		break;
	case OutputKind::Stream: {
		OBSDataAutoRelease ss = obs_data_create();
		obs_data_set_string(ss, "server", spec.server.c_str());
		obs_data_set_string(ss, "key", spec.key.c_str());
		OBSServiceAutoRelease service =
			obs_service_create_private("rtmp_custom", (spec.name + "-service").c_str(), ss);
		output_id = service ? stream_output_type(service) : nullptr;
		if (!service || !output_id) {
			error = obs_module_text("Error.StreamServer");
			return nullptr;
		}
		out->service_ = service.Get();
		out->path_ = spec.server;
		break;
	}
	}

	OBSOutputAutoRelease output = obs_output_create(output_id, spec.name.c_str(), settings, nullptr);
	if (!output) {
		error = obs_module_text("Error.Output");
		return nullptr;
	}
	out->output_ = output.Get();

	obs_output_set_video_encoder(output, out->encoders_->video());
	/* A stream carries one audio track; files carry every encoded track. */
	const auto &audio = out->encoders_->audio();
	const size_t tracks = kind == OutputKind::Stream ? std::min<size_t>(1, audio.size()) : audio.size();
	for (size_t i = 0; i < tracks; i++)
		obs_output_set_audio_encoder(output, audio[i], i);

	if (kind == OutputKind::Stream) {
		obs_output_set_service(output, out->service_);
		obs_output_set_reconnect_settings(output, 25, 2);
		obs_output_set_delay(output, 0, 0);
	}

	out->context_.reset(new SignalContext{token, output.Get()});
	SignalContext *ctx = out->context_.get();
	out->signals_.reserve(3);
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	out->signals_.emplace_back(sh, "stop",
				   [](void *p, calldata_t *cd) {
					   auto *c = static_cast<SignalContext *>(p);
					   const char *err = obs_output_get_last_error(c->output);
					   post(c->token, OutputEvent::Stopped, (int)calldata_int(cd, "code"),
						err ? err : "");
				   },
				   ctx);
	if (kind == OutputKind::Replay)
		out->signals_.emplace_back(sh, "saved",
					   [](void *p, calldata_t *) {
						   auto *c = static_cast<SignalContext *>(p);
						   calldata_t cd = {};
						   proc_handler_call(obs_output_get_proc_handler(c->output),
								     "get_last_replay", &cd);
						   const char *path = calldata_string(&cd, "path");
						   post(c->token, OutputEvent::ReplaySaved, 0, path ? path : "");
						   calldata_free(&cd);
					   },
					   ctx);
	if (kind == OutputKind::Stream) {
		out->signals_.emplace_back(sh, "reconnect",
					   [](void *p, calldata_t *) {
						   post(static_cast<SignalContext *>(p)->token, OutputEvent::Reconnecting,
							0, {});
					   },
					   ctx);
		out->signals_.emplace_back(sh, "reconnect_success",
					   [](void *p, calldata_t *) {
						   post(static_cast<SignalContext *>(p)->token, OutputEvent::Reconnected,
							0, {});
					   },
					   ctx);
	}

	if (!obs_output_start(output)) {
		const char *detail = obs_output_get_last_error(output);
		error = std::string(obs_module_text("Error.Start")) + (detail && *detail ? std::string(": ") + detail : "");
		out->signals_.clear();
		return nullptr;
	}

	out->started_ns_ = os_gettime_ns();
	blog(LOG_INFO, "[video-mixes] Started %s '%s' (%s, %ux%u)",
	     kind == OutputKind::Record ? "recording" : kind == OutputKind::Replay ? "replay buffer" : "stream",
	     spec.name.c_str(), obs_encoder_get_id(out->encoders_->video()), obs_encoder_get_width(out->encoders_->video()),
	     obs_encoder_get_height(out->encoders_->video()));
	return out;
}

MixOutput::~MixOutput()
{
	signals_.clear();
	if (output_ && obs_output_active(output_))
		obs_output_force_stop(output_);
}

void MixOutput::stop()
{
	obs_output_stop(output_);
}

bool MixOutput::pause(bool paused)
{
	return kind_ == OutputKind::Record && obs_output_pause(output_, paused);
}

bool MixOutput::save_replay()
{
	if (kind_ != OutputKind::Replay || !obs_output_active(output_))
		return false;
	calldata_t cd = {};
	const bool ok = proc_handler_call(obs_output_get_proc_handler(output_), "save", &cd);
	calldata_free(&cd);
	return ok;
}

std::string MixOutput::last_replay() const
{
	if (kind_ != OutputKind::Replay)
		return {};
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(output_), "get_last_replay", &cd);
	const char *path = calldata_string(&cd, "path");
	std::string out = path ? path : "";
	calldata_free(&cd);
	return out;
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
	case OBS_OUTPUT_CONNECT_FAILED:
		key = "Error.ConnectFailed";
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		key = "Error.InvalidStream";
		break;
	case OBS_OUTPUT_DISCONNECTED:
		key = "Error.Disconnected";
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
