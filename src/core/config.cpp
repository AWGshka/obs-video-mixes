#include "core/config.hpp"
#include "core/encoders.hpp"
#include "core/util.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <algorithm>

namespace vmix {

namespace {

constexpr const char *kFile = "mixes.json";

/* 1: v0.1.0. 2: encoder selection by capabilities + constant-quality defaults. 3: replay buffer, stream, source
 * audio, hotkeys as an object. */
constexpr int kSchema = 3;

void use_encoder(MixConfig &m, const std::string &id)
{
	blog(LOG_INFO, "[video-mixes] Mix '%s': encoder %s → %s", m.name.c_str(),
	     m.encoder_id.empty() ? "(none)" : m.encoder_id.c_str(), id.c_str());
	m.encoder_id = id;
	m.encoder_settings = adopt(recording_defaults(id.c_str()));
}

/* Repairs settings written by older versions or for encoders that are gone; returns true if anything changed. */
bool migrate(MixConfig &m, int schema)
{
	const std::string preferred = preferred_video_encoder();
	bool changed = false;

	/* Deprecated/renamed (e.g. jim_nvenc) or no longer available encoder. */
	if (!encoder_selectable(m.encoder_id.c_str())) {
		use_encoder(m, preferred);
		changed = true;
	} else if (schema < 2 && m.encoder_id == "obs_x264" && preferred != "obs_x264") {
		/* v0.1.0 fell back to x264 because it looked for outdated NVENC ids; move such mixes to the GPU. */
		use_encoder(m, preferred);
		changed = true;
	} else if (!obs_data_has_user_value(m.encoder_settings, "rate_control") &&
		   apply_quality_defaults(m.encoder_id.c_str(), m.encoder_settings)) {
		/* Rate control never set explicitly means the encoder default, usually CBR: switch to constant quality. */
		blog(LOG_INFO, "[video-mixes] Mix '%s': rate control → %s", m.name.c_str(),
		     obs_data_get_string(m.encoder_settings, "rate_control"));
		changed = true;
	}

	if (!encoder_selectable(m.stream_encoder_id.c_str())) {
		m.stream_encoder_id = preferred;
		m.stream_encoder_settings = adopt(stream_defaults(preferred.c_str()));
		changed = true;
	}
	return changed;
}

OBSDataArray array_or_empty(obs_data_t *data, const char *key)
{
	obs_data_array_t *arr = obs_data_get_array(data, key);
	return arr ? adopt(arr) : new_array();
}

OBSData object_or_empty(obs_data_t *data, const char *key)
{
	obs_data_t *obj = obs_data_get_obj(data, key);
	return obj ? adopt(obj) : new_data();
}

MixConfig mix_from_data(obs_data_t *d, int schema)
{
	/* Keys missing in older files take these values. */
	obs_data_set_default_bool(d, "record", true);
	obs_data_set_default_int(d, "trigger", (int)Trigger::Recording);
	obs_data_set_default_int(d, "audio_tracks", 1);
	obs_data_set_default_int(d, "audio_bitrate", 192);
	obs_data_set_default_int(d, "replay_trigger", (int)Trigger::ReplayBuffer);
	obs_data_set_default_int(d, "replay_seconds", 30);
	obs_data_set_default_int(d, "replay_mb", 512);
	obs_data_set_default_bool(d, "replay_save_with_obs", true);
	obs_data_set_default_int(d, "stream_trigger", (int)Trigger::Streaming);
	obs_data_set_default_int(d, "stream_audio_track", 1);
	obs_data_set_default_int(d, "stream_audio_bitrate", 160);

	MixConfig m;
	m.id = (uint32_t)obs_data_get_int(d, "id");
	m.name = obs_data_get_string(d, "name");
	m.kind = (MixKind)obs_data_get_int(d, "kind");
	m.source_uuid = obs_data_get_string(d, "source_uuid");
	m.source_name = obs_data_get_string(d, "source_name");
	m.width = (uint32_t)obs_data_get_int(d, "width");
	m.height = (uint32_t)obs_data_get_int(d, "height");
	m.fps_divisor = (uint32_t)obs_data_get_int(d, "fps_divisor");
	m.transitions = (TransitionStyle)obs_data_get_int(d, "transitions");

	m.record = obs_data_get_bool(d, "record");
	m.trigger = (Trigger)obs_data_get_int(d, "trigger");
	m.container = (Container)obs_data_get_int(d, "container");
	m.folder = obs_data_get_string(d, "folder");
	m.encoder_id = obs_data_get_string(d, "encoder_id");
	m.encoder_settings = object_or_empty(d, "encoder_settings");
	m.audio_tracks = (uint32_t)obs_data_get_int(d, "audio_tracks") & ((1u << kAudioTracks) - 1);
	m.audio_bitrate = (int)obs_data_get_int(d, "audio_bitrate");
	m.audio_source = (AudioSource)obs_data_get_int(d, "audio_source");

	m.replay = obs_data_get_bool(d, "replay");
	m.replay_trigger = (Trigger)obs_data_get_int(d, "replay_trigger");
	m.replay_seconds = (int)obs_data_get_int(d, "replay_seconds");
	m.replay_mb = (int)obs_data_get_int(d, "replay_mb");
	m.replay_save_with_obs = obs_data_get_bool(d, "replay_save_with_obs");

	m.stream = obs_data_get_bool(d, "stream");
	m.stream_trigger = (Trigger)obs_data_get_int(d, "stream_trigger");
	m.stream_server = obs_data_get_string(d, "stream_server");
	m.stream_key = obs_data_get_string(d, "stream_key");
	m.stream_encoder_id = obs_data_get_string(d, "stream_encoder_id");
	m.stream_encoder_settings = object_or_empty(d, "stream_encoder_settings");
	m.stream_audio_track = std::clamp((int)obs_data_get_int(d, "stream_audio_track"), 1, kAudioTracks);
	m.stream_audio_bitrate = (int)obs_data_get_int(d, "stream_audio_bitrate");

	OBSData hotkeys = object_or_empty(d, "hotkeys");
	if (schema < 3) {
		obs_data_set_array(hotkeys, "Start", array_or_empty(d, "hotkey_start"));
		obs_data_set_array(hotkeys, "Stop", array_or_empty(d, "hotkey_stop"));
	}
	for (const char *name : kHotkeyNames)
		m.hotkeys[name] = array_or_empty(hotkeys, name);

	if (!m.audio_tracks)
		m.audio_tracks = 1;
	m.audio_bitrate = std::max(32, m.audio_bitrate);
	m.stream_audio_bitrate = std::max(32, m.stream_audio_bitrate);
	m.replay_seconds = std::clamp(m.replay_seconds, 5, 21600);
	m.replay_mb = std::clamp(m.replay_mb, 16, 32768);
	return m;
}

OBSDataAutoRelease mix_to_data(const MixConfig &m)
{
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_int(d, "id", m.id);
	obs_data_set_string(d, "name", m.name.c_str());
	obs_data_set_int(d, "kind", (int)m.kind);
	obs_data_set_string(d, "source_uuid", m.source_uuid.c_str());
	obs_data_set_string(d, "source_name", m.source_name.c_str());
	obs_data_set_int(d, "width", m.width);
	obs_data_set_int(d, "height", m.height);
	obs_data_set_int(d, "fps_divisor", m.fps_divisor);
	obs_data_set_int(d, "transitions", (int)m.transitions);

	obs_data_set_bool(d, "record", m.record);
	obs_data_set_int(d, "trigger", (int)m.trigger);
	obs_data_set_int(d, "container", (int)m.container);
	obs_data_set_string(d, "folder", m.folder.c_str());
	obs_data_set_string(d, "encoder_id", m.encoder_id.c_str());
	obs_data_set_obj(d, "encoder_settings", m.encoder_settings);
	obs_data_set_int(d, "audio_tracks", m.audio_tracks);
	obs_data_set_int(d, "audio_bitrate", m.audio_bitrate);
	obs_data_set_int(d, "audio_source", (int)m.audio_source);

	obs_data_set_bool(d, "replay", m.replay);
	obs_data_set_int(d, "replay_trigger", (int)m.replay_trigger);
	obs_data_set_int(d, "replay_seconds", m.replay_seconds);
	obs_data_set_int(d, "replay_mb", m.replay_mb);
	obs_data_set_bool(d, "replay_save_with_obs", m.replay_save_with_obs);

	obs_data_set_bool(d, "stream", m.stream);
	obs_data_set_int(d, "stream_trigger", (int)m.stream_trigger);
	obs_data_set_string(d, "stream_server", m.stream_server.c_str());
	obs_data_set_string(d, "stream_key", m.stream_key.c_str());
	obs_data_set_string(d, "stream_encoder_id", m.stream_encoder_id.c_str());
	obs_data_set_obj(d, "stream_encoder_settings", m.stream_encoder_settings);
	obs_data_set_int(d, "stream_audio_track", m.stream_audio_track);
	obs_data_set_int(d, "stream_audio_bitrate", m.stream_audio_bitrate);

	OBSDataAutoRelease hotkeys = obs_data_create();
	for (const auto &[name, bindings] : m.hotkeys)
		if (bindings)
			obs_data_set_array(hotkeys, name.c_str(), bindings);
	obs_data_set_obj(d, "hotkeys", hotkeys);
	return d;
}

} // namespace

MixConfig default_mix(uint32_t id, const std::string &name, MixKind kind)
{
	MixConfig m;
	m.id = id;
	m.name = name;
	m.kind = kind;
	m.encoder_id = preferred_video_encoder();
	m.encoder_settings = adopt(recording_defaults(m.encoder_id.c_str()));
	m.stream_encoder_id = m.encoder_id;
	m.stream_encoder_settings = adopt(stream_defaults(m.stream_encoder_id.c_str()));
	if (kind == MixKind::Source)
		m.audio_source = AudioSource::Source;
	for (const char *hk : kHotkeyNames)
		m.hotkeys[hk] = new_array();
	return m;
}

const char *container_extension(Container c)
{
	switch (c) {
	case Container::Mp4:
	case Container::HybridMp4:
		return "mp4";
	case Container::Mov:
	case Container::HybridMov:
		return "mov";
	case Container::Mkv:
	default:
		return "mkv";
	}
}

Config Config::load()
{
	Config cfg;
	BPtr<char> path = obs_module_config_path(kFile);
	OBSDataAutoRelease root = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	if (!root)
		return cfg;

	cfg.next_id = (uint32_t)std::max<long long>(1, obs_data_get_int(root, "next_id"));
	const int schema = (int)obs_data_get_int(root, "schema");
	cfg.migrated = schema < kSchema;
	OBSDataArrayAutoRelease arr = obs_data_get_array(root, "mixes");
	for (size_t i = 0, n = obs_data_array_count(arr); i < n; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		MixConfig m = mix_from_data(item, schema);
		if (migrate(m, schema))
			cfg.migrated = true;
		const bool duplicate = std::any_of(cfg.mixes.begin(), cfg.mixes.end(),
						   [&](const MixConfig &o) { return o.id == m.id; });
		if (m.id >= 1 && m.id <= kMaxMixId && !duplicate)
			cfg.mixes.push_back(std::move(m));
	}
	return cfg;
}

void Config::save() const
{
	BPtr<char> dir = obs_module_config_path("");
	if (dir)
		os_mkdirs(dir);
	BPtr<char> path = obs_module_config_path(kFile);
	if (!path)
		return;

	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_int(root, "schema", kSchema);
	obs_data_set_int(root, "next_id", next_id);
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const MixConfig &m : mixes)
		obs_data_array_push_back(arr, mix_to_data(m));
	obs_data_set_array(root, "mixes", arr);
	if (!obs_data_save_json_pretty_safe(root, path, "tmp", "bak"))
		blog(LOG_WARNING, "[video-mixes] Failed to save %s", path.Get());
}

uint32_t Config::allocate_id()
{
	auto used = [&](uint32_t id) {
		return std::any_of(mixes.begin(), mixes.end(), [&](const MixConfig &m) { return m.id == id; });
	};

	/* Ids are not reused while possible: stale visibility bits may remain in scene collections
	 * that were not loaded when a mix was deleted. */
	for (uint32_t id = next_id; id <= kMaxMixId; id++) {
		if (!used(id)) {
			next_id = id + 1;
			return id;
		}
	}
	for (uint32_t id = 1; id <= kMaxMixId; id++)
		if (!used(id))
			return id;
	return 0;
}

} // namespace vmix
