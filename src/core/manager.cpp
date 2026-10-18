#include "core/manager.hpp"
#include "core/frame-rate.hpp"
#include "core/obs-recording.hpp"
#include "core/util.hpp"
#include "core/visibility.hpp"

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace vmix {

namespace {

constexpr const char *kOutputNames[] = {"recording", "replay buffer", "stream"};

std::string format1(const char *key, const std::string &arg)
{
	std::string s = obs_module_text(key);
	if (auto pos = s.find("%1"); pos != std::string::npos)
		s.replace(pos, 2, arg);
	return s;
}

std::string sanitize_filename(const std::string &name)
{
	std::string out;
	for (char c : name)
		if (c != '%')
			out += std::strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32 ? '_' : c;
	return out.empty() ? std::string("Mix") : out;
}

void split_path(const std::string &path, std::string &dir, std::string &stem)
{
	const size_t slash = path.find_last_of("/\\");
	dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
	std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
	const size_t dot = file.find_last_of('.');
	stem = dot == std::string::npos ? file : file.substr(0, dot);
}

std::string filename_format()
{
	config_t *profile = obs_frontend_get_profile_config();
	const char *fmt = profile ? config_get_string(profile, "Output", "FilenameFormatting") : nullptr;
	return fmt && *fmt ? fmt : "%CCYY-%MM-%DD %hh-%mm-%ss";
}

std::string default_folder(const MixConfig &cfg)
{
	if (!cfg.folder.empty())
		return cfg.folder;
	BPtr<char> rec_dir = obs_frontend_get_current_record_output_path();
	return rec_dir ? rec_dir.Get() : ".";
}

/* Hotkey callback data: mix id and the index into kHotkeyNames. */
void *pack(uint32_t id, size_t action)
{
	return reinterpret_cast<void *>(static_cast<uintptr_t>((id << 8) | (uint32_t)action));
}

/* Hotkeys fire on the hotkey thread; everything in Manager is UI-thread only. */
void on_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			const auto v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
			const uint32_t id = v >> 8;
			Manager &m = Manager::get();
			switch (v & 0xff) {
			case 0:
				m.start_output(id, OutputKind::Record);
				break;
			case 1:
				m.stop_output(id, OutputKind::Record);
				break;
			case 2:
				m.start_output(id, OutputKind::Replay);
				break;
			case 3:
				m.stop_output(id, OutputKind::Replay);
				break;
			case 4:
				m.save_replay(id);
				break;
			case 5:
				m.start_output(id, OutputKind::Stream);
				break;
			case 6:
				m.stop_output(id, OutputKind::Stream);
				break;
			default:
				break;
			}
		},
		data, false);
}

bool hotkey_wanted(const MixConfig &cfg, size_t action)
{
	switch (action) {
	case 0:
	case 1:
		return cfg.record;
	case 2:
	case 3:
	case 4:
		return cfg.replay;
	default:
		return cfg.stream;
	}
}

void frontend_event_cb(enum obs_frontend_event event, void *)
{
	Manager::get().handle_frontend_event(event);
}

void frontend_save_cb(obs_data_t *, bool saving, void *)
{
	if (saving)
		Manager::get().persist();
}

} // namespace

Manager &Manager::get()
{
	static Manager instance;
	return instance;
}

bool Manager::Runtime::any_output() const
{
	return std::any_of(outputs.begin(), outputs.end(), [](const Output &o) { return o.out != nullptr; });
}

void Manager::init()
{
	if (initialized_)
		return;
	initialized_ = true;
	released_ = false;

	Config cfg = Config::load();
	next_id_ = cfg.next_id;
	for (MixConfig &m : cfg.mixes) {
		auto rt = std::make_unique<Runtime>();
		rt->cfg = std::move(m);
		sync_hotkeys(*rt);
		mixes_.push_back(std::move(rt));
	}

	set_output_event_handler([](uint64_t token, OutputEvent event, int code, const std::string &text) {
		Manager::get().on_output_event(token, event, code, text);
	});

	obs_frontend_add_event_callback(frontend_event_cb, nullptr);
	obs_frontend_add_save_callback(frontend_save_cb, nullptr);

	OBSCanvasAutoRelease main = obs_get_main_canvas();
	if (main)
		video_reset_signal_.Connect(
			obs_canvas_get_signal_handler(main), "video_reset",
			[](void *, calldata_t *) {
				obs_queue_task(OBS_TASK_UI, [](void *) { Manager::get().on_video_reset(); }, nullptr,
					       false);
			},
			nullptr);

	if (cfg.migrated)
		save();
	blog(LOG_INFO, "[video-mixes] Loaded %zu mix(es)", mixes_.size());
	notify();
}

void Manager::release_all()
{
	if (!initialized_ || released_)
		return;
	save();
	released_ = true; /* later save callbacks must not overwrite the config with an empty list */
	for (auto &rt : mixes_) {
		for (Output &o : rt->outputs)
			o.out.reset();
		rt->canvas.reset();
		unregister_hotkeys(*rt);
	}
	mixes_.clear();
	notify();
}

void Manager::shutdown()
{
	release_all();
	if (!initialized_)
		return;
	obs_frontend_remove_event_callback(frontend_event_cb, nullptr);
	obs_frontend_remove_save_callback(frontend_save_cb, nullptr);
	video_reset_signal_.Disconnect();
	set_output_event_handler(nullptr);
	listeners_.clear();
	replay_listeners_.clear();
	initialized_ = false;
}

std::vector<MixState> Manager::states() const
{
	std::vector<MixState> out;
	out.reserve(mixes_.size());
	for (const auto &rt : mixes_) {
		MixState s;
		s.cfg = rt->cfg;
		s.error = rt->error;
		s.previewing = rt->preview_users > 0;
		s.fps = rt->any_output() ? rt->fps : 0.0;
		for (int k = 0; k < kOutputKinds; k++) {
			const Output &o = rt->outputs[k];
			OutputState &st = s.outputs[k];
			st.error = o.error;
			st.path = o.last_path;
			if (!o.out)
				continue;
			st.active = true;
			st.origin = o.origin;
			st.paused = o.out->paused();
			st.reconnecting = o.reconnecting;
			st.started_ns = o.out->started_ns();
			st.dropped_frames = o.out->dropped_frames();
			st.total_frames = o.out->total_frames();
			st.total_bytes = o.out->total_bytes();
			st.congestion = o.out->congestion();
			if ((OutputKind)k != OutputKind::Replay)
				st.path = o.out->path();
		}
		out.push_back(std::move(s));
	}
	return out;
}

std::vector<uint32_t> Manager::ids() const
{
	std::vector<uint32_t> out;
	for (const auto &rt : mixes_)
		out.push_back(rt->cfg.id);
	return out;
}

const MixConfig *Manager::find(uint32_t id) const
{
	const Runtime *rt = runtime(id);
	return rt ? &rt->cfg : nullptr;
}

uint32_t Manager::find_by_name(const std::string &name) const
{
	for (const auto &rt : mixes_)
		if (rt->cfg.name == name)
			return rt->cfg.id;
	return 0;
}

Manager::Runtime *Manager::runtime(uint32_t id) const
{
	auto it = std::find_if(mixes_.begin(), mixes_.end(), [&](const auto &rt) { return rt->cfg.id == id; });
	return it == mixes_.end() ? nullptr : it->get();
}

MixConfig Manager::new_mix(const std::string &name, MixKind kind) const
{
	MixConfig cfg = default_mix(0, name, kind);
	/* Same encoder, bitrate, audio and format as OBS recordings. */
	if (inherit_obs_recording(cfg))
		blog(LOG_INFO, "[video-mixes] New mix '%s' inherits OBS recording settings: %s %s, tracks 0x%x, %d kbps audio",
		     cfg.name.c_str(), cfg.encoder_id.c_str(), obs_data_get_json(cfg.encoder_settings), cfg.audio_tracks,
		     cfg.audio_bitrate);
	inherit_obs_stream(cfg);
	return cfg;
}

uint32_t Manager::add_mix(MixConfig cfg)
{
	Config tmp;
	tmp.next_id = next_id_;
	for (const auto &rt : mixes_)
		tmp.mixes.push_back(rt->cfg);
	const uint32_t id = tmp.allocate_id();
	if (!id)
		return 0;
	next_id_ = tmp.next_id;

	auto rt = std::make_unique<Runtime>();
	cfg.id = id;
	rt->cfg = std::move(cfg);
	sync_hotkeys(*rt);
	mixes_.push_back(std::move(rt));
	save();
	notify();
	return id;
}

uint32_t Manager::duplicate_mix(uint32_t id, const std::string &name)
{
	const Runtime *rt = runtime(id);
	if (!rt)
		return 0;
	MixConfig copy = rt->cfg;
	copy.name = name;
	/* Deep copies: settings objects must not be shared between mixes. */
	copy.encoder_settings = new_data();
	obs_data_apply(copy.encoder_settings, rt->cfg.encoder_settings);
	copy.stream_encoder_settings = new_data();
	obs_data_apply(copy.stream_encoder_settings, rt->cfg.stream_encoder_settings);
	copy.hotkeys.clear();
	const uint32_t new_id = add_mix(std::move(copy));
	/* Program mixes: start with the same hidden items. */
	if (new_id && rt->cfg.kind == MixKind::Program)
		copy_bit_everywhere(rt->cfg.bit(), 1u << new_id);
	return new_id;
}

void Manager::update_mix(const MixConfig &cfg)
{
	Runtime *rt = runtime(cfg.id);
	if (!rt)
		return;

	MixConfig next = cfg;
	next.hotkeys = rt->cfg.hotkeys;

	const bool renamed = next.name != rt->cfg.name;
	const bool resized = next.width != rt->cfg.width || next.height != rt->cfg.height ||
			     next.source_uuid != rt->cfg.source_uuid;
	const bool fade_changed = next.transitions != rt->cfg.transitions;
	rt->cfg = std::move(next);

	if (renamed)
		unregister_hotkeys(*rt); /* re-registered below with the new name */
	sync_hotkeys(*rt);
	if (rt->canvas && fade_changed)
		rt->canvas->set_fade(rt->cfg.transitions == TransitionStyle::Fade);
	if (rt->canvas && resized && !rt->any_output()) {
		rt->canvas.reset();
		if (rt->preview_users > 0)
			ensure_canvas(*rt);
	}
	save();
	notify();
}

void Manager::remove_mix(uint32_t id)
{
	auto it = std::find_if(mixes_.begin(), mixes_.end(), [&](const auto &rt) { return rt->cfg.id == id; });
	if (it == mixes_.end())
		return;
	Runtime &rt = **it;
	for (Output &o : rt.outputs)
		o.out.reset();
	rt.canvas.reset();
	unregister_hotkeys(rt);
	clear_bit_everywhere(rt.cfg.bit());
	mixes_.erase(it);
	save();
	notify();
}

MixCanvas *Manager::ensure_canvas(Runtime &rt)
{
	if (!rt.canvas) {
		std::string error;
		rt.canvas = MixCanvas::create(rt.cfg, error);
		rt.error = rt.canvas ? std::string() : error;
		if (!rt.canvas)
			blog(LOG_WARNING, "[video-mixes] Mix '%s': %s", rt.cfg.name.c_str(), error.c_str());
	}
	return rt.canvas.get();
}

void Manager::release_if_unused(Runtime &rt)
{
	if (!rt.any_output() && rt.preview_users <= 0)
		rt.canvas.reset();
}

bool Manager::output_enabled(const MixConfig &cfg, OutputKind kind) const
{
	switch (kind) {
	case OutputKind::Record:
		return cfg.record;
	case OutputKind::Replay:
		return cfg.replay;
	case OutputKind::Stream:
		return cfg.stream;
	}
	return false;
}

Trigger Manager::output_trigger(const MixConfig &cfg, OutputKind kind) const
{
	switch (kind) {
	case OutputKind::Record:
		return cfg.trigger;
	case OutputKind::Replay:
		return cfg.replay_trigger;
	case OutputKind::Stream:
		return cfg.stream_trigger;
	}
	return Trigger::Manual;
}

std::shared_ptr<EncoderSet> Manager::encoders_for(Runtime &rt, OutputKind kind, MixCanvas &canvas, std::string &error)
{
	std::weak_ptr<EncoderSet> &slot = kind == OutputKind::Stream ? rt.stream_encoders : rt.file_encoders;
	if (auto existing = slot.lock())
		return existing; /* recording and replay buffer share one encode */

	const MixConfig &cfg = rt.cfg;
	OBSSourceAutoRelease source =
		cfg.kind == MixKind::Source ? obs_get_source_by_uuid(cfg.source_uuid.c_str()) : nullptr;
	const FrameRateChoice rate = resolve_frame_rate(cfg, source);

	EncoderSpec spec;
	spec.name = "video-mixes-" + std::to_string(cfg.id) + (kind == OutputKind::Stream ? "-stream" : "-file");
	if (kind == OutputKind::Stream) {
		spec.encoder_id = cfg.stream_encoder_id;
		spec.encoder_settings = cfg.stream_encoder_settings;
		spec.audio_tracks = 1u << (cfg.stream_audio_track - 1);
		spec.audio_bitrate = cfg.stream_audio_bitrate;
	} else {
		spec.encoder_id = cfg.encoder_id;
		spec.encoder_settings = cfg.encoder_settings;
		spec.audio_tracks = cfg.audio_tracks;
		spec.audio_bitrate = cfg.audio_bitrate;
	}
	if (cfg.source_audio())
		spec.source_audio = source;

	auto set = EncoderSet::create(spec, canvas.video(), rate.divisor, error);
	if (set) {
		slot = set;
		rt.fps = rate.fps;
	}
	return set;
}

OutputSpec Manager::output_spec(const MixConfig &cfg, OutputKind kind, Trigger origin) const
{
	OutputSpec spec;
	spec.name = "video-mixes-" + std::to_string(cfg.id) + "-" + kOutputNames[(int)kind];
	spec.container = cfg.container;
	switch (kind) {
	case OutputKind::Record:
		spec.path = build_path(cfg, origin);
		break;
	case OutputKind::Replay:
		spec.directory = default_folder(cfg);
		os_mkdirs(spec.directory.c_str());
		spec.filename_format = "Replay " + filename_format() + " (" + sanitize_filename(cfg.name) + ")";
		spec.extension = container_extension(cfg.container);
		spec.max_seconds = cfg.replay_seconds;
		spec.max_mb = cfg.replay_mb;
		break;
	case OutputKind::Stream:
		spec.server = cfg.stream_server;
		spec.key = cfg.stream_key;
		break;
	}
	return spec;
}

bool Manager::start_output(uint32_t id, OutputKind kind, Trigger origin)
{
	Runtime *rt = runtime(id);
	if (!rt || rt->out(kind).out || !output_enabled(rt->cfg, kind))
		return false;
	Output &o = rt->out(kind);

	auto fail = [&](const std::string &error) {
		o.error = error;
		blog(LOG_WARNING, "[video-mixes] Mix '%s' %s failed to start: %s", rt->cfg.name.c_str(),
		     kOutputNames[(int)kind], error.c_str());
		release_if_unused(*rt);
		notify();
		return false;
	};

	if (kind == OutputKind::Stream && rt->cfg.stream_server.empty())
		return fail(obs_module_text("Error.StreamServer"));

	MixCanvas *canvas = ensure_canvas(*rt);
	if (!canvas)
		return fail(rt->error);

	std::string error;
	std::shared_ptr<EncoderSet> encoders = encoders_for(*rt, kind, *canvas, error);
	if (!encoders)
		return fail(error);

	const uint64_t token = next_token_++;
	o.out = MixOutput::start(kind, output_spec(rt->cfg, kind, origin), std::move(encoders), token, error);
	if (!o.out)
		return fail(error);

	o.token = token;
	o.origin = origin;
	o.reconnecting = false;
	o.error.clear();
	if (kind == OutputKind::Record && origin == Trigger::Recording && obs_frontend_recording_paused())
		o.out->pause(true);
	notify();
	return true;
}

void Manager::stop_output(uint32_t id, OutputKind kind)
{
	if (Runtime *rt = runtime(id); rt && rt->out(kind).out)
		rt->out(kind).out->stop();
}

bool Manager::output_active(uint32_t id, OutputKind kind) const
{
	const Runtime *rt = runtime(id);
	return rt && rt->out(kind).out;
}

bool Manager::any_active(uint32_t id) const
{
	const Runtime *rt = runtime(id);
	return rt && rt->any_output();
}

bool Manager::save_replay(uint32_t id)
{
	Runtime *rt = runtime(id);
	return rt && rt->out(OutputKind::Replay).out && rt->out(OutputKind::Replay).out->save_replay();
}

std::string Manager::output_error(uint32_t id, OutputKind kind) const
{
	const Runtime *rt = runtime(id);
	return rt ? rt->out(kind).error : std::string();
}

obs_canvas_t *Manager::acquire_preview(uint32_t id)
{
	Runtime *rt = runtime(id);
	if (!rt)
		return nullptr;
	rt->preview_users++;
	if (MixCanvas *c = ensure_canvas(*rt)) {
		notify();
		return c->canvas();
	}
	rt->preview_users--;
	notify();
	return nullptr;
}

void Manager::release_preview(uint32_t id)
{
	if (Runtime *rt = runtime(id)) {
		rt->preview_users = std::max(0, rt->preview_users - 1);
		release_if_unused(*rt);
		notify();
	}
}

obs_canvas_t *Manager::canvas(uint32_t id) const
{
	const Runtime *rt = runtime(id);
	return rt && rt->canvas ? rt->canvas->canvas() : nullptr;
}

int Manager::subscribe(Listener fn)
{
	const int handle = next_listener_++;
	listeners_.emplace_back(handle, std::move(fn));
	return handle;
}

void Manager::unsubscribe(int handle)
{
	std::erase_if(listeners_, [&](const auto &l) { return l.first == handle; });
}

int Manager::subscribe_replays(ReplayListener fn)
{
	const int handle = next_listener_++;
	replay_listeners_.emplace_back(handle, std::move(fn));
	return handle;
}

void Manager::unsubscribe_replays(int handle)
{
	std::erase_if(replay_listeners_, [&](const auto &l) { return l.first == handle; });
}

void Manager::notify()
{
	/* Copy: a listener may unsubscribe while being notified. */
	auto listeners = listeners_;
	for (auto &[handle, fn] : listeners)
		fn();
}

void Manager::save()
{
	if (!initialized_ || released_)
		return;
	Config cfg;
	cfg.next_id = next_id_;
	for (const auto &rt : mixes_) {
		MixConfig m = rt->cfg;
		for (const auto &[name, hk] : rt->hotkeys)
			m.hotkeys[name] = adopt(obs_hotkey_save(hk));
		cfg.mixes.push_back(std::move(m));
	}
	cfg.save();
}

/* Registers the hotkeys of the outputs a mix uses and unregisters the others, keeping their bindings. */
void Manager::sync_hotkeys(Runtime &rt)
{
	const std::string id = std::to_string(rt.cfg.id);
	for (size_t a = 0; a < std::size(kHotkeyNames); a++) {
		const std::string name = kHotkeyNames[a];
		auto it = rt.hotkeys.find(name);
		const bool wanted = hotkey_wanted(rt.cfg, a);
		if (it != rt.hotkeys.end() && !wanted) {
			rt.cfg.hotkeys[name] = adopt(obs_hotkey_save(it->second));
			obs_hotkey_unregister(it->second);
			rt.hotkeys.erase(it);
		} else if (it == rt.hotkeys.end() && wanted) {
			const std::string description = format1(("Hotkey." + name).c_str(), rt.cfg.name);
			const obs_hotkey_id hk = obs_hotkey_register_frontend(("VideoMixes." + name + "." + id).c_str(),
									     description.c_str(), on_hotkey,
									     pack(rt.cfg.id, a));
			if (auto saved = rt.cfg.hotkeys.find(name); saved != rt.cfg.hotkeys.end() && saved->second)
				obs_hotkey_load(hk, saved->second);
			rt.hotkeys[name] = hk;
		}
	}
}

void Manager::unregister_hotkeys(Runtime &rt)
{
	for (const auto &[name, hk] : rt.hotkeys) {
		rt.cfg.hotkeys[name] = adopt(obs_hotkey_save(hk));
		obs_hotkey_unregister(hk);
	}
	rt.hotkeys.clear();
}

void Manager::start_triggered(Trigger trigger)
{
	for (auto &rt : mixes_)
		for (int k = 0; k < kOutputKinds; k++)
			if (output_enabled(rt->cfg, (OutputKind)k) && output_trigger(rt->cfg, (OutputKind)k) == trigger &&
			    !rt->outputs[k].out)
				start_output(rt->cfg.id, (OutputKind)k, trigger);
}

void Manager::stop_triggered(Trigger trigger)
{
	for (auto &rt : mixes_)
		for (Output &o : rt->outputs)
			if (o.out && o.origin == trigger)
				o.out->stop();
}

void Manager::handle_frontend_event(enum obs_frontend_event event)
{
	auto pause_all = [&](bool paused) {
		for (auto &rt : mixes_) {
			Output &o = rt->out(OutputKind::Record);
			if (o.out && o.origin == Trigger::Recording)
				o.out->pause(paused);
		}
		notify();
	};

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		start_triggered(Trigger::Recording);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		stop_triggered(Trigger::Recording);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		pause_all(true);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		pause_all(false);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		start_triggered(Trigger::Streaming);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stop_triggered(Trigger::Streaming);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		start_triggered(Trigger::ReplayBuffer);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		stop_triggered(Trigger::ReplayBuffer);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		for (auto &rt : mixes_)
			if (rt->cfg.replay_save_with_obs)
				save_replay(rt->cfg.id);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		release_all();
		break;
	default:
		break;
	}
}

void Manager::on_output_event(uint64_t token, OutputEvent event, int code, const std::string &text)
{
	for (auto &rt : mixes_) {
		for (int k = 0; k < kOutputKinds; k++) {
			Output &o = rt->outputs[k];
			if (!o.out || o.token != token)
				continue;
			switch (event) {
			case OutputEvent::Stopped:
				blog(LOG_INFO, "[video-mixes] Mix '%s' %s stopped (code %d)", rt->cfg.name.c_str(),
				     kOutputNames[k], code);
				if ((OutputKind)k == OutputKind::Record)
					o.last_path = o.out->path();
				o.out.reset();
				o.reconnecting = false;
				o.error = output_error_message(code, text);
				release_if_unused(*rt);
				break;
			case OutputEvent::ReplaySaved: {
				o.last_path = text;
				blog(LOG_INFO, "[video-mixes] Mix '%s' replay saved: %s", rt->cfg.name.c_str(),
				     text.c_str());
				auto listeners = replay_listeners_;
				for (auto &[handle, fn] : listeners)
					fn(rt->cfg.id, text);
				break;
			}
			case OutputEvent::Reconnecting:
				o.reconnecting = true;
				break;
			case OutputEvent::Reconnected:
				o.reconnecting = false;
				break;
			}
			notify();
			return;
		}
	}
}

void Manager::on_video_reset()
{
	for (auto &rt : mixes_) {
		if (!rt->canvas || rt->any_output())
			continue;
		rt->canvas.reset();
		if (rt->preview_users > 0)
			ensure_canvas(*rt);
	}
	notify();
}

std::string Manager::build_path(const MixConfig &cfg, Trigger origin) const
{
	std::string dir = cfg.folder;
	std::string stem;

	/* Alongside the main recording: same timestamped name plus the mix name, easy to pair in an editor. */
	if (origin == Trigger::Recording) {
		OBSOutputAutoRelease main = obs_frontend_get_recording_output();
		if (main) {
			OBSDataAutoRelease settings = obs_output_get_settings(main);
			std::string path = obs_data_get_string(settings, "path");
			if (path.empty())
				path = obs_data_get_string(settings, "url");
			if (!path.empty()) {
				std::string main_dir;
				split_path(path, main_dir, stem);
				if (dir.empty())
					dir = main_dir;
			}
		}
	}

	if (dir.empty())
		dir = default_folder(cfg);
	if (stem.empty()) {
		BPtr<char> name = os_generate_formatted_filename(nullptr, true, filename_format().c_str());
		stem = name ? name.Get() : "recording";
	}

	os_mkdirs(dir.c_str());
	const std::string base = dir + "/" + stem + " (" + sanitize_filename(cfg.name) + ")";
	const std::string ext = std::string(".") + container_extension(cfg.container);
	std::string path = base + ext;
	for (int n = 2; os_file_exists(path.c_str()); n++)
		path = base + " " + std::to_string(n) + ext;
	return path;
}

} // namespace vmix
