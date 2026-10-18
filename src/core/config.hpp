#pragma once

#include <obs.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vmix {

/* Bit 0 of a visibility mask is reserved; mixes use ids 1..31. */
constexpr uint32_t kMaxMixId = 31;
constexpr int kAudioTracks = 6;

/* Program: the program output minus hidden items. Source: one source by itself at its own resolution. */
enum class MixKind : int { Program = 0, Source = 1 };
enum class Trigger : int { Manual = 0, Recording = 1, Streaming = 2 };
enum class Container : int { Mkv = 0, Mp4 = 1, Mov = 2, HybridMp4 = 3, HybridMov = 4 };
enum class TransitionStyle : int { Fade = 0, Cut = 1 };

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

	Trigger trigger = Trigger::Recording;
	TransitionStyle transitions = TransitionStyle::Fade;

	Container container = Container::Mkv;
	std::string folder; /* empty = OBS recording folder */

	std::string encoder_id;
	OBSData encoder_settings;

	uint32_t audio_tracks = 1u; /* bit n = mixer track n+1 */
	int audio_bitrate = 192;

	OBSDataArray hotkey_start;
	OBSDataArray hotkey_stop;

	uint32_t bit() const { return 1u << id; }
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
