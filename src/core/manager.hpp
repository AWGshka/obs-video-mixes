#pragma once

#include "core/config.hpp"
#include "core/mix-canvas.hpp"
#include "core/outputs.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vmix {

struct OutputState {
	bool active = false;
	bool paused = false;
	bool reconnecting = false;
	Trigger origin = Trigger::Manual;
	std::string path; /* record: the file; replay: the last saved file; stream: the server */
	uint64_t started_ns = 0;
	int dropped_frames = 0;
	int total_frames = 0;
	uint64_t total_bytes = 0;
	float congestion = 0.0f;
	std::string error; /* why the last start failed / the output stopped; cleared on start */
};

struct MixState {
	MixConfig cfg;
	std::array<OutputState, kOutputKinds> outputs;
	double fps = 0.0; /* output frame rate while any output runs */
	bool previewing = false;
	std::string error; /* canvas could not be created */

	const OutputState &out(OutputKind k) const { return outputs[(size_t)k]; }
	bool any_active() const
	{
		for (const OutputState &o : outputs)
			if (o.active)
				return true;
		return false;
	}
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
	std::vector<uint32_t> ids() const;
	const MixConfig *find(uint32_t id) const;
	uint32_t find_by_name(const std::string &name) const;

	/* Settings for a new mix: defaults plus OBS' own recording/stream setup. Not added until add_mix(). */
	MixConfig new_mix(const std::string &name, MixKind kind) const;
	/* Adds a mix (cfg.id is assigned); returns its id, or 0 if all ids are taken. */
	uint32_t add_mix(MixConfig cfg);
	uint32_t add_mix(const std::string &name, MixKind kind) { return add_mix(new_mix(name, kind)); }
	/* Copy of a mix with a new name; outputs off-trigger as configured. Returns the new id or 0. */
	uint32_t duplicate_mix(uint32_t id, const std::string &name);
	void update_mix(const MixConfig &cfg);
	void remove_mix(uint32_t id);

	bool start_output(uint32_t id, OutputKind kind, Trigger origin = Trigger::Manual);
	void stop_output(uint32_t id, OutputKind kind);
	bool output_active(uint32_t id, OutputKind kind) const;
	bool any_active(uint32_t id) const;
	bool save_replay(uint32_t id);
	/* Why the last start of this output failed, if it did. */
	std::string output_error(uint32_t id, OutputKind kind) const;

	/* Preview users keep the mix canvas alive; returns nullptr if the canvas could not be created.
	 * The canvas may be recreated (video reset, resolution change): re-query canvas() on notify. */
	obs_canvas_t *acquire_preview(uint32_t id);
	void release_preview(uint32_t id);
	obs_canvas_t *canvas(uint32_t id) const;

	using Listener = std::function<void()>;
	int subscribe(Listener fn);
	void unsubscribe(int handle);

	/* Called after a replay of a mix was saved (path of the file). */
	using ReplayListener = std::function<void(uint32_t id, const std::string &path)>;
	int subscribe_replays(ReplayListener fn);
	void unsubscribe_replays(int handle);

	/* Entry points for frontend callbacks. */
	void handle_frontend_event(enum obs_frontend_event event);
	void persist() { save(); }

private:
	struct Output {
		std::unique_ptr<MixOutput> out;
		Trigger origin = Trigger::Manual;
		uint64_t token = 0;
		bool reconnecting = false;
		std::string error;
		std::string last_path;
	};

	struct Runtime {
		MixConfig cfg;
		std::unique_ptr<MixCanvas> canvas;
		int preview_users = 0;
		std::array<Output, kOutputKinds> outputs;
		std::weak_ptr<EncoderSet> file_encoders;   /* recording + replay buffer */
		std::weak_ptr<EncoderSet> stream_encoders;
		double fps = 0.0;
		std::string error;
		std::map<std::string, obs_hotkey_id> hotkeys;

		/* Stopped outputs, released after a grace period (see retire()). They keep the canvas alive. */
		std::vector<std::pair<uint64_t, std::unique_ptr<MixOutput>>> retired;

		Output &out(OutputKind k) { return outputs[(size_t)k]; }
		const Output &out(OutputKind k) const { return outputs[(size_t)k]; }
		bool any_output() const;  /* live outputs */
		bool holds_canvas() const { return any_output() || !retired.empty(); }
	};

	Manager() = default;

	Runtime *runtime(uint32_t id) const;
	MixCanvas *ensure_canvas(Runtime &rt);
	void release_if_unused(Runtime &rt);
	std::shared_ptr<EncoderSet> encoders_for(Runtime &rt, OutputKind kind, MixCanvas &canvas, std::string &error);
	bool output_enabled(const MixConfig &cfg, OutputKind kind) const;
	Trigger output_trigger(const MixConfig &cfg, OutputKind kind) const;
	OutputSpec output_spec(const MixConfig &cfg, OutputKind kind, Trigger origin) const;

	void sync_hotkeys(Runtime &rt);
	void unregister_hotkeys(Runtime &rt);
	void save();
	void notify();

	void on_output_event(uint64_t token, OutputEvent event, int code, const std::string &text);
	void on_video_reset();
	void retire(Runtime &rt, std::unique_ptr<MixOutput> out);
	void reap(bool all);
	static void reap_tick(void *param, float seconds);
	void start_triggered(Trigger trigger);
	void stop_triggered(Trigger trigger);
	std::string build_path(const MixConfig &cfg, Trigger origin) const;

	bool initialized_ = false;
	bool released_ = false;
	uint32_t next_id_ = 1;
	uint64_t next_token_ = 1;
	std::vector<std::unique_ptr<Runtime>> mixes_;
	std::vector<std::pair<int, Listener>> listeners_;
	std::vector<std::pair<int, ReplayListener>> replay_listeners_;
	int next_listener_ = 1;
	OBSSignal video_reset_signal_;
	bool reap_tick_registered_ = false;
	std::atomic<bool> reap_queued_{false};
};

} // namespace vmix
