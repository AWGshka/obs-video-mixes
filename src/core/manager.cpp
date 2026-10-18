#include "core/manager.hpp"
#include "core/util.hpp"
#include "core/visibility.hpp"
#include "core/frame-rate.hpp"
#include "core/obs-recording.hpp"

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <algorithm>
#include <cstring>

namespace vmix {

namespace {

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

void *pack(uint32_t id)
{
	return reinterpret_cast<void *>(static_cast<uintptr_t>(id));
}

uint32_t unpack(void *p)
{
	return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

/* Hotkeys fire on the hotkey thread; everything in Manager is UI-thread only. */
void on_hotkey_start(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		obs_queue_task(
			OBS_TASK_UI, [](void *p) { Manager::get().start_recording(unpack(p), Trigger::Manual); }, data,
			false);
}

void on_hotkey_stop(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		obs_queue_task(OBS_TASK_UI, [](void *p) { Manager::get().stop_recording(unpack(p)); }, data, false);
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

void Manager::init()
{
	if (initialized_)
		return;
	initialized_ = true;

	Config cfg = Config::load();
	next_id_ = cfg.next_id;
	for (MixConfig &m : cfg.mixes) {
		auto rt = std::make_unique<Runtime>();
		rt->cfg = std::move(m);
		register_hotkeys(*rt);
		mixes_.push_back(std::move(rt));
	}

	set_recording_stopped_handler([](uint64_t token, int code, const std::string &error) {
		Manager::get().on_recording_stopped(token, code, error);
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
		rt->rec.reset();
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
	set_recording_stopped_handler(nullptr);
	listeners_.clear();
	initialized_ = false;
}

std::vector<MixState> Manager::states() const
{
	std::vector<MixState> out;
	out.reserve(mixes_.size());
	for (const auto &rt : mixes_) {
		MixState s;
		s.cfg = rt->cfg;
		s.last_error = rt->last_error;
		if (rt->rec) {
			s.recording = true;
			s.paused = rt->rec->paused();
			s.origin = rt->origin;
			s.path = rt->rec->path();
			s.started_ns = rt->rec->started_ns();
			s.dropped_frames = rt->rec->dropped_frames();
			s.fps = rt->fps;
			s.total_frames = rt->rec->total_frames();
		}
		out.push_back(std::move(s));
	}
	return out;
}

const MixConfig *Manager::find(uint32_t id) const
{
	const Runtime *rt = runtime(id);
	return rt ? &rt->cfg : nullptr;
}

Manager::Runtime *Manager::runtime(uint32_t id) const
{
	auto it = std::find_if(mixes_.begin(), mixes_.end(), [&](const auto &rt) { return rt->cfg.id == id; });
	return it == mixes_.end() ? nullptr : it->get();
}

uint32_t Manager::add_mix(const std::string &name, MixKind kind)
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
	rt->cfg = default_mix(id, name, kind);
	/* Same encoder, bitrate, audio and format as OBS recordings. */
	if (inherit_obs_recording(rt->cfg))
		blog(LOG_INFO, "[video-mixes] Mix '%s' inherits OBS recording settings: %s %s, tracks 0x%x, %d kbps audio",
		     rt->cfg.name.c_str(), rt->cfg.encoder_id.c_str(), obs_data_get_json(rt->cfg.encoder_settings),
		     rt->cfg.audio_tracks, rt->cfg.audio_bitrate);
	register_hotkeys(*rt);
	mixes_.push_back(std::move(rt));
	save();
	notify();
	return id;
}

void Manager::update_mix(const MixConfig &cfg)
{
	Runtime *rt = runtime(cfg.id);
	if (!rt)
		return;

	MixConfig next = cfg;
	next.hotkey_start = rt->cfg.hotkey_start;
	next.hotkey_stop = rt->cfg.hotkey_stop;

	const bool renamed = next.name != rt->cfg.name;
	const bool resized = next.width != rt->cfg.width || next.height != rt->cfg.height ||
			     next.source_uuid != rt->cfg.source_uuid;
	const bool fade_changed = next.transitions != rt->cfg.transitions;
	rt->cfg = std::move(next);

	if (renamed) {
		obs_hotkey_set_description(rt->hk_start, format1("Hotkey.Start", rt->cfg.name).c_str());
		obs_hotkey_set_description(rt->hk_stop, format1("Hotkey.Stop", rt->cfg.name).c_str());
	}
	if (rt->canvas && fade_changed)
		rt->canvas->set_fade(rt->cfg.transitions == TransitionStyle::Fade);
	if (rt->canvas && resized && !rt->rec) {
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
	rt.rec.reset();
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
		if (!rt.canvas) {
			rt.last_error = error;
			blog(LOG_WARNING, "[video-mixes] Mix '%s': %s", rt.cfg.name.c_str(), error.c_str());
		}
	}
	return rt.canvas.get();
}

void Manager::release_canvas_if_unused(Runtime &rt)
{
	if (!rt.rec && rt.preview_users <= 0)
		rt.canvas.reset();
}

bool Manager::start_recording(uint32_t id, Trigger origin)
{
	Runtime *rt = runtime(id);
	if (!rt || rt->rec)
		return false;

	MixCanvas *canvas = ensure_canvas(*rt);
	if (!canvas) {
		notify();
		return false;
	}

	OBSSourceAutoRelease source =
		rt->cfg.kind == MixKind::Source ? obs_get_source_by_uuid(rt->cfg.source_uuid.c_str()) : nullptr;
	const FrameRateChoice rate = resolve_frame_rate(rt->cfg, source);

	const std::string path = build_path(rt->cfg, origin);
	const uint64_t token = next_token_++;
	std::string error;
	rt->rec = MixRecording::start(rt->cfg, canvas->video(), rate.divisor, path, token, error);
	if (!rt->rec) {
		rt->last_error = error;
		blog(LOG_WARNING, "[video-mixes] Mix '%s' failed to start: %s", rt->cfg.name.c_str(), error.c_str());
		release_canvas_if_unused(*rt);
		notify();
		return false;
	}

	rt->token = token;
	rt->origin = origin;
	rt->fps = rate.fps;
	rt->last_error.clear();
	if (origin == Trigger::Recording && obs_frontend_recording_paused())
		rt->rec->pause(true);
	notify();
	return true;
}

void Manager::stop_recording(uint32_t id)
{
	if (Runtime *rt = runtime(id); rt && rt->rec)
		rt->rec->stop();
}

bool Manager::recording(uint32_t id) const
{
	const Runtime *rt = runtime(id);
	return rt && rt->rec;
}

obs_canvas_t *Manager::acquire_preview(uint32_t id)
{
	Runtime *rt = runtime(id);
	if (!rt)
		return nullptr;
	rt->preview_users++;
	if (MixCanvas *c = ensure_canvas(*rt))
		return c->canvas();
	rt->preview_users--;
	notify();
	return nullptr;
}

void Manager::release_preview(uint32_t id)
{
	if (Runtime *rt = runtime(id)) {
		rt->preview_users = std::max(0, rt->preview_users - 1);
		release_canvas_if_unused(*rt);
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
		if (rt->hk_start != OBS_INVALID_HOTKEY_ID)
			m.hotkey_start = adopt(obs_hotkey_save(rt->hk_start));
		if (rt->hk_stop != OBS_INVALID_HOTKEY_ID)
			m.hotkey_stop = adopt(obs_hotkey_save(rt->hk_stop));
		cfg.mixes.push_back(std::move(m));
	}
	cfg.save();
}

void Manager::register_hotkeys(Runtime &rt)
{
	const std::string id = std::to_string(rt.cfg.id);
	rt.hk_start = obs_hotkey_register_frontend(("VideoMixes.Start." + id).c_str(),
						   format1("Hotkey.Start", rt.cfg.name).c_str(), on_hotkey_start,
						   pack(rt.cfg.id));
	rt.hk_stop = obs_hotkey_register_frontend(("VideoMixes.Stop." + id).c_str(),
						  format1("Hotkey.Stop", rt.cfg.name).c_str(), on_hotkey_stop,
						  pack(rt.cfg.id));
	if (rt.cfg.hotkey_start)
		obs_hotkey_load(rt.hk_start, rt.cfg.hotkey_start);
	if (rt.cfg.hotkey_stop)
		obs_hotkey_load(rt.hk_stop, rt.cfg.hotkey_stop);
}

void Manager::unregister_hotkeys(Runtime &rt)
{
	if (rt.hk_start != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(rt.hk_start);
	if (rt.hk_stop != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(rt.hk_stop);
	rt.hk_start = rt.hk_stop = OBS_INVALID_HOTKEY_ID;
}

void Manager::for_trigger(Trigger trigger, const std::function<void(Runtime &)> &fn)
{
	for (auto &rt : mixes_)
		if (rt->cfg.trigger == trigger)
			fn(*rt);
}

void Manager::handle_frontend_event(enum obs_frontend_event event)
{
	auto start_all = [&](Trigger t) {
		for_trigger(t, [&](Runtime &rt) {
			if (!rt.rec)
				start_recording(rt.cfg.id, t);
		});
	};
	auto stop_all = [&](Trigger origin) {
		for (auto &rt : mixes_)
			if (rt->rec && rt->origin == origin)
				rt->rec->stop();
	};
	auto pause_all = [&](bool paused) {
		for (auto &rt : mixes_)
			if (rt->rec && rt->origin == Trigger::Recording)
				rt->rec->pause(paused);
		notify();
	};

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		start_all(Trigger::Recording);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		stop_all(Trigger::Recording);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		pause_all(true);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		pause_all(false);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		start_all(Trigger::Streaming);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stop_all(Trigger::Streaming);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		release_all();
		break;
	default:
		break;
	}
}

void Manager::on_recording_stopped(uint64_t token, int code, const std::string &error)
{
	auto it = std::find_if(mixes_.begin(), mixes_.end(),
			       [&](const auto &rt) { return rt->rec && rt->token == token; });
	if (it == mixes_.end())
		return;
	Runtime &rt = **it;
	blog(LOG_INFO, "[video-mixes] Mix '%s' recording stopped (code %d)", rt.cfg.name.c_str(), code);
	rt.rec.reset();
	rt.last_error = output_error_message(code, error);
	release_canvas_if_unused(rt);
	notify();
}

void Manager::on_video_reset()
{
	for (auto &rt : mixes_) {
		if (!rt->canvas || rt->rec)
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

	if (dir.empty()) {
		BPtr<char> rec_dir = obs_frontend_get_current_record_output_path();
		dir = rec_dir ? rec_dir.Get() : ".";
	}
	if (stem.empty()) {
		config_t *profile = obs_frontend_get_profile_config();
		const char *fmt = profile ? config_get_string(profile, "Output", "FilenameFormatting") : nullptr;
		BPtr<char> name = os_generate_formatted_filename(nullptr, true,
								 fmt && *fmt ? fmt : "%CCYY-%MM-%DD %hh-%mm-%ss");
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
