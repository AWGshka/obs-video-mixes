#include "core/websocket.hpp"
#include "core/manager.hpp"
#include "core/visibility.hpp"

#include "third-party/obs-websocket-api.h"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vmix {

namespace {

constexpr const char *kVendor = "video-mixes";
constexpr std::array kOutputIds = {"record", "replay", "stream"};
constexpr std::array kTriggerIds = {"manual", "recording", "streaming", "replay_buffer"};

obs_websocket_vendor g_vendor = nullptr;
int g_listener = 0;
int g_replay_listener = 0;
std::vector<std::string> g_registered;

/* Last reported (active, paused) per mix and output, to emit OutputStateChanged only on changes. */
struct Reported {
	uint32_t id;
	int output;
	bool active;
	bool paused;
};
std::vector<Reported> g_reported;

using Handler = std::function<void(obs_data_t *request, obs_data_t *response)>;

/* obs-websocket calls vendor requests on its own threads; the Manager lives on the UI thread. */
void run_on_ui(const Handler &handler, obs_data_t *request, obs_data_t *response)
{
	struct Call {
		const Handler *handler;
		obs_data_t *request;
		obs_data_t *response;
	} call{&handler, request, response};
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			auto *c = static_cast<Call *>(p);
			(*c->handler)(c->request, c->response);
		},
		&call, true);
}

void fail(obs_data_t *response, const std::string &message)
{
	obs_data_set_bool(response, "ok", false);
	obs_data_set_string(response, "error", message.c_str());
}

void succeed(obs_data_t *response)
{
	obs_data_set_bool(response, "ok", true);
}

/* Resolves "mixId" (number) or "mixName" (string). */
uint32_t find_mix(obs_data_t *request, obs_data_t *response)
{
	Manager &m = Manager::get();
	if (obs_data_has_user_value(request, "mixId")) {
		const uint32_t id = (uint32_t)obs_data_get_int(request, "mixId");
		if (m.find(id))
			return id;
		fail(response, "No mix with mixId " + std::to_string(id));
		return 0;
	}
	const char *name = obs_data_get_string(request, "mixName");
	if (!name || !*name) {
		fail(response, "Specify mixName or mixId");
		return 0;
	}
	if (const uint32_t id = m.find_by_name(name))
		return id;
	fail(response, std::string("No mix named '") + name + "'");
	return 0;
}

bool find_output(obs_data_t *request, obs_data_t *response, OutputKind &kind)
{
	const char *output = obs_data_get_string(request, "output");
	for (size_t i = 0; i < kOutputIds.size(); i++) {
		if (output && std::strcmp(output, kOutputIds[i]) == 0) {
			kind = (OutputKind)i;
			return true;
		}
	}
	fail(response, "output must be \"record\", \"replay\" or \"stream\"");
	return false;
}

OBSDataAutoRelease mix_to_data(const MixState &s)
{
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_int(d, "mixId", s.cfg.id);
	obs_data_set_string(d, "mixName", s.cfg.name.c_str());
	obs_data_set_string(d, "kind", s.cfg.kind == MixKind::Source ? "source" : "program");
	obs_data_set_string(d, "sourceName", s.cfg.source_name.c_str());
	obs_data_set_bool(d, "previewing", s.previewing);

	const std::array<bool, kOutputKinds> enabled{s.cfg.record, s.cfg.replay, s.cfg.stream};
	const std::array<Trigger, kOutputKinds> triggers{s.cfg.trigger, s.cfg.replay_trigger, s.cfg.stream_trigger};
	OBSDataAutoRelease outputs = obs_data_create();
	for (int k = 0; k < kOutputKinds; k++) {
		const OutputState &o = s.outputs[k];
		OBSDataAutoRelease out = obs_data_create();
		obs_data_set_bool(out, "enabled", enabled[k]);
		obs_data_set_bool(out, "active", o.active);
		obs_data_set_bool(out, "paused", o.paused);
		obs_data_set_bool(out, "reconnecting", o.reconnecting);
		obs_data_set_string(out, "trigger", kTriggerIds[(size_t)triggers[k] < kTriggerIds.size() ? (size_t)triggers[k] : 0]);
		obs_data_set_string(out, "path", o.path.c_str());
		obs_data_set_string(out, "error", o.error.c_str());
		obs_data_set_obj(outputs, kOutputIds[k], out);
	}
	obs_data_set_obj(d, "outputs", outputs);
	return d;
}

const MixState *state_of(const std::vector<MixState> &states, uint32_t id)
{
	for (const MixState &s : states)
		if (s.cfg.id == id)
			return &s;
	return nullptr;
}

/* sceneName + sceneItemId (any depth via groups) or sourceName (searched recursively). */
OBSSceneItem find_item(obs_data_t *request, obs_data_t *response)
{
	const char *scene_name = obs_data_get_string(request, "sceneName");
	OBSSourceAutoRelease scene_source = obs_get_source_by_name(scene_name ? scene_name : "");
	obs_scene_t *scene = scene_source ? obs_group_or_scene_from_source(scene_source) : nullptr;
	if (!scene) {
		fail(response, std::string("No scene named '") + (scene_name ? scene_name : "") + "'");
		return nullptr;
	}
	obs_sceneitem_t *item = nullptr;
	if (obs_data_has_user_value(request, "sceneItemId"))
		item = obs_scene_find_sceneitem_by_id(scene, obs_data_get_int(request, "sceneItemId"));
	else if (const char *source = obs_data_get_string(request, "sourceName"); source && *source)
		item = obs_scene_find_source_recursive(scene, source);
	if (!item)
		fail(response, "Scene item not found (use sceneItemId or sourceName)");
	return item;
}

/* ---- requests ---- */

const std::vector<std::pair<const char *, Handler>> &requests()
{
	static const std::vector<std::pair<const char *, Handler>> list = {
		{"GetMixList",
		 [](obs_data_t *, obs_data_t *res) {
			 OBSDataArrayAutoRelease arr = obs_data_array_create();
			 for (const MixState &s : Manager::get().states())
				 obs_data_array_push_back(arr, mix_to_data(s));
			 obs_data_set_array(res, "mixes", arr);
			 succeed(res);
		 }},
		{"GetMix",
		 [](obs_data_t *req, obs_data_t *res) {
			 const uint32_t id = find_mix(req, res);
			 if (!id)
				 return;
			 const auto states = Manager::get().states();
			 if (const MixState *s = state_of(states, id))
				 obs_data_apply(res, mix_to_data(*s));
			 succeed(res);
		 }},
		{"StartOutput",
		 [](obs_data_t *req, obs_data_t *res) {
			 OutputKind kind;
			 const uint32_t id = find_mix(req, res);
			 if (!id || !find_output(req, res, kind))
				 return;
			 Manager &m = Manager::get();
			 if (m.output_active(id, kind))
				 return succeed(res);
			 if (m.start_output(id, kind))
				 return succeed(res);
			 const std::string error = m.output_error(id, kind);
			 fail(res, error.empty() ? std::string("The output is disabled in the mix settings") : error);
		 }},
		{"StopOutput",
		 [](obs_data_t *req, obs_data_t *res) {
			 OutputKind kind;
			 const uint32_t id = find_mix(req, res);
			 if (!id || !find_output(req, res, kind))
				 return;
			 Manager::get().stop_output(id, kind);
			 succeed(res);
		 }},
		{"ToggleOutput",
		 [](obs_data_t *req, obs_data_t *res) {
			 OutputKind kind;
			 const uint32_t id = find_mix(req, res);
			 if (!id || !find_output(req, res, kind))
				 return;
			 Manager &m = Manager::get();
			 if (m.output_active(id, kind)) {
				 m.stop_output(id, kind);
				 obs_data_set_bool(res, "active", false);
				 return succeed(res);
			 }
			 if (!m.start_output(id, kind)) {
				 const std::string error = m.output_error(id, kind);
				 return fail(res, error.empty() ? std::string("The output is disabled in the mix settings")
								: error);
			 }
			 obs_data_set_bool(res, "active", true);
			 succeed(res);
		 }},
		{"SaveReplay",
		 [](obs_data_t *req, obs_data_t *res) {
			 const uint32_t id = find_mix(req, res);
			 if (!id)
				 return;
			 if (Manager::get().save_replay(id))
				 succeed(res);
			 else
				 fail(res, "The mix's replay buffer is not running");
		 }},
		{"GetItemVisibility",
		 [](obs_data_t *req, obs_data_t *res) {
			 OBSSceneItem item = find_item(req, res);
			 if (!item)
				 return;
			 const uint32_t mask = hidden_mask(item);
			 OBSDataArrayAutoRelease arr = obs_data_array_create();
			 for (const MixState &s : Manager::get().states()) {
				 if (s.cfg.kind != MixKind::Program)
					 continue;
				 OBSDataAutoRelease d = obs_data_create();
				 obs_data_set_int(d, "mixId", s.cfg.id);
				 obs_data_set_string(d, "mixName", s.cfg.name.c_str());
				 obs_data_set_bool(d, "visible", !(mask & s.cfg.bit()));
				 obs_data_array_push_back(arr, d);
			 }
			 obs_data_set_array(res, "mixes", arr);
			 succeed(res);
		 }},
		{"SetItemVisibility",
		 [](obs_data_t *req, obs_data_t *res) {
			 OBSSceneItem item = find_item(req, res);
			 if (!item)
				 return;
			 const uint32_t id = find_mix(req, res);
			 if (!id)
				 return;
			 const MixConfig *cfg = Manager::get().find(id);
			 if (cfg->kind != MixKind::Program)
				 return fail(res, "Only program mixes hide scene items");
			 if (!obs_data_has_user_value(req, "visible"))
				 return fail(res, "Specify visible: true or false");
			 set_hidden(item, cfg->bit(), !obs_data_get_bool(req, "visible"));
			 /* The dock listens to scene signals; nudge it like OBS does after an item change. */
			 obs_scene_t *scene = obs_sceneitem_get_scene(item);
			 signal_handler_signal(obs_source_get_signal_handler(obs_scene_get_source(scene)), "refresh", nullptr);
			 succeed(res);
		 }},
	};
	return list;
}

void on_request(obs_data_t *request, obs_data_t *response, void *param)
{
	run_on_ui(*static_cast<const Handler *>(param), request, response);
}

/* ---- events (UI thread) ---- */

void emit_changes()
{
	if (!g_vendor)
		return;
	const auto states = Manager::get().states();
	std::vector<Reported> now;
	for (const MixState &s : states) {
		for (int k = 0; k < kOutputKinds; k++) {
			const OutputState &o = s.outputs[k];
			now.push_back({s.cfg.id, k, o.active, o.paused});
			auto prev = std::find_if(g_reported.begin(), g_reported.end(),
						 [&](const Reported &r) { return r.id == s.cfg.id && r.output == k; });
			/* New mixes report only outputs that already run; known ones report every change. */
			const bool changed = prev == g_reported.end() ? o.active
								       : prev->active != o.active || prev->paused != o.paused;
			if (!changed)
				continue;
			OBSDataAutoRelease ev = obs_data_create();
			obs_data_set_int(ev, "mixId", s.cfg.id);
			obs_data_set_string(ev, "mixName", s.cfg.name.c_str());
			obs_data_set_string(ev, "output", kOutputIds[k]);
			obs_data_set_bool(ev, "active", o.active);
			obs_data_set_bool(ev, "paused", o.paused);
			obs_data_set_string(ev, "path", o.path.c_str());
			obs_websocket_vendor_emit_event(g_vendor, "OutputStateChanged", ev);
		}
	}
	g_reported = std::move(now);
}

void emit_replay_saved(uint32_t id, const std::string &path)
{
	const MixConfig *cfg = Manager::get().find(id);
	if (!g_vendor || !cfg)
		return;
	OBSDataAutoRelease ev = obs_data_create();
	obs_data_set_int(ev, "mixId", id);
	obs_data_set_string(ev, "mixName", cfg->name.c_str());
	obs_data_set_string(ev, "path", path.c_str());
	obs_websocket_vendor_emit_event(g_vendor, "ReplaySaved", ev);
}

void unregister_requests()
{
	if (!g_vendor)
		return;
	Manager::get().unsubscribe(g_listener);
	Manager::get().unsubscribe_replays(g_replay_listener);
	for (const std::string &name : g_registered)
		obs_websocket_vendor_unregister_request(g_vendor, name.c_str());
	g_registered.clear();
	g_reported.clear();
	g_vendor = nullptr;
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	/* Before modules unload: obs-websocket may be gone by the time this module unloads. */
	if (event == OBS_FRONTEND_EVENT_EXIT)
		unregister_requests();
}

} // namespace

void register_websocket_vendor()
{
	if (g_vendor)
		return;
	g_vendor = obs_websocket_register_vendor(kVendor);
	if (!g_vendor) {
		blog(LOG_INFO, "[video-mixes] obs-websocket not available; vendor requests disabled");
		return;
	}
	for (const auto &[name, handler] : requests()) {
		if (obs_websocket_vendor_register_request(g_vendor, name, on_request, const_cast<Handler *>(&handler)))
			g_registered.emplace_back(name);
		else
			blog(LOG_WARNING, "[video-mixes] Failed to register websocket request %s", name);
	}
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	g_listener = Manager::get().subscribe(emit_changes);
	g_replay_listener = Manager::get().subscribe_replays(emit_replay_saved);
	emit_changes(); /* baseline */
	blog(LOG_INFO, "[video-mixes] obs-websocket vendor '%s' registered (%zu requests)", kVendor, g_registered.size());
}

void unregister_websocket_vendor()
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	unregister_requests();
}

} // namespace vmix
