#pragma once

#include <obs.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vmix {

/* Bit 0 of a visibility mask is reserved; mixes use ids 1..31. */
constexpr uint32_t kMaxMixId = 31;
constexpr int kAudioTracks = 6;

/* Program: the program output minus hidden items. Source: one source by itself at its own resolution. */
enum class MixKind : int { Program = 0, Source = 1 };
/* When an output starts and stops: together with an OBS output, or only by hand (dock, hotkey, websocket). */
enum class Trigger : int { Manual = 0, Recording = 1, Streaming = 2, ReplayBuffer = 3 };
enum class Container : int { Mkv = 0, Mp4 = 1, Mov = 2, HybridMp4 = 3, HybridMov = 4 };
enum class TransitionStyle : int { Fade = 0, Cut = 1 };
/* Audio of a source mix: the selected mixer tracks, or only the source's own audio. */
enum class AudioSource : int { Tracks = 0, Source = 1 };

/* Hotkey ids per mix (registered as "VideoMixes.<name>.<mix id>"). */
inline constexpr const char *kHotkeyNames[] = {"Start", "Stop", "ReplayStart", "ReplayStop",
					       "ReplaySave", "StreamStart", "StreamStop"};

struct MixConfig {
	uint32_t id = 0;
	std::string name;
	MixKind kind = MixKind::Program;

	/* Source mixes: the recorded source (uuid is stable across renames; name is for display if missing). */
	std::string source_uuid;
	std::string source_name;

	/* 0 = follow the OBS output resolution (program mixes) / the source's own size (source mixes). */
	uint32_t width = 0;
	uint32_t height = 0;

	/* Output fps = OBS fps / divisor. 0 = auto: the source's own frame rate (source mixes) or OBS fps. */
	uint32_t fps_divisor = 0;

	TransitionStyle transitions = TransitionStyle::Fade;

	/* ---- file recording (and replay buffer: same encoders, format and folder) ---- */
	bool record = true;
	Trigger trigger = Trigger::Recording;
	Container container = Container::Mkv;
	std::string folder; /* empty = OBS recording folder */
	std::string encoder_id;
	OBSData encoder_settings;
	uint32_t audio_tracks = 1u; /* bit n = mixer track n+1 */
	int audio_bitrate = 192;
	AudioSource audio_source = AudioSource::Tracks;

	/* ---- replay buffer ---- */
	bool replay = false;
	Trigger replay_trigger = Trigger::ReplayBuffer;
	int replay_seconds = 30;
	int replay_mb = 512;
	bool replay_save_with_obs = true; /* saving OBS' replay buffer also saves this mix's */

	/* ---- stream ---- */
	bool stream = false;
	Trigger stream_trigger = Trigger::Streaming;
	std::string stream_server;
	std::string stream_key;
	std::string stream_encoder_id;
	OBSData stream_encoder_settings;
	int stream_audio_track = 1; /* 1-based mixer track */
	int stream_audio_bitrate = 160;

	std::map<std::string, OBSDataArray> hotkeys; /* saved bindings by kHotkeyNames entry */

	uint32_t bit() const { return 1u << id; }
	bool source_audio() const { return kind == MixKind::Source && audio_source == AudioSource::Source; }
};

struct Config {
	std::vector<MixConfig> mixes;
	uint32_t next_id = 1;
	bool migrated = false; /* load() upgraded old settings; caller should save */

	static Config load();
	void save() const;

	uint32_t allocate_id();
};

MixConfig default_mix(uint32_t id, const std::string &name, MixKind kind = MixKind::Program);
const char *container_extension(Container c);

} // namespace vmix
