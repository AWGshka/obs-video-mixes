#pragma once

#include "core/config.hpp"
#include "core/mix-canvas.hpp"
#include "core/recording.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vmix {

struct MixState {
	MixConfig cfg;
	bool recording = false;
	bool paused = false;
	Trigger origin = Trigger::Manual;
	std::string path;
	uint64_t started_ns = 0;
	int dropped_frames = 0;
	int total_frames = 0;
	double fps = 0.0; /* output frame rate while recording */
	std::string last_error;
};

/* Owns all mixes and their runtime objects. UI-thread only. */
class Manager {
public:
	static Manager &get();

	void init();
	/* Stops outputs and frees canvases/hotkeys; safe to call repeatedly (EXIT event, module unload). */
	void release_all();
	void shutdown();

	std::vector<MixState> states() const;
	const MixConfig *find(uint32_t id) const;

	uint32_t add_mix(const std::string &name, MixKind kind);
	void update_mix(const MixConfig &cfg);
	void remove_mix(uint32_t id);

	bool start_recording(uint32_t id, Trigger origin = Trigger::Manual);
	void stop_recording(uint32_t id);
	bool recording(uint32_t id) const;

	/* Preview users keep the mix canvas alive; returns nullptr if the canvas could not be created.
	 * The canvas may be recreated (video reset, resolution change): re-query canvas() on notify. */
	obs_canvas_t *acquire_preview(uint32_t id);
	void release_preview(uint32_t id);
	obs_canvas_t *canvas(uint32_t id) const;

	using Listener = std::function<void()>;
	int subscribe(Listener fn);
	void unsubscribe(int handle);

	/* Entry points for frontend callbacks. */
	void handle_frontend_event(enum obs_frontend_event event);
	void persist() { save(); }

private:
	struct Runtime {
		MixConfig cfg;
		std::unique_ptr<MixCanvas> canvas;
		int preview_users = 0;
		std::unique_ptr<MixRecording> rec;
		Trigger origin = Trigger::Manual;
		uint64_t token = 0;
		double fps = 0.0;
		std::string last_error;
		obs_hotkey_id hk_start = OBS_INVALID_HOTKEY_ID;
		obs_hotkey_id hk_stop = OBS_INVALID_HOTKEY_ID;
	};

	Manager() = default;

	Runtime *runtime(uint32_t id) const;
	MixCanvas *ensure_canvas(Runtime &rt);
	void release_canvas_if_unused(Runtime &rt);
	void register_hotkeys(Runtime &rt);
	void unregister_hotkeys(Runtime &rt);
	void save();
	void notify();

	void on_recording_stopped(uint64_t token, int code, const std::string &error);
	void on_video_reset();
	void for_trigger(Trigger trigger, const std::function<void(Runtime &)> &fn);
	std::string build_path(const MixConfig &cfg, Trigger origin) const;

	bool initialized_ = false;
	bool released_ = false;
	uint32_t next_id_ = 1;
	uint64_t next_token_ = 1;
	std::vector<std::unique_ptr<Runtime>> mixes_;
	std::vector<std::pair<int, Listener>> listeners_;
	int next_listener_ = 1;
	OBSSignal video_reset_signal_;
};

} // namespace vmix
