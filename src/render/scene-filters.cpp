#include "render/scene-filters.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vmix {

namespace {

constexpr const char *kProxyId = "video_mixes_scene_proxy";

/* Copies of a scene that is no longer drawn are dropped after this many frames (~2 s at 60 fps). */
constexpr uint64_t kKeepFrames = 120;

/* Private data of a proxy source. */
struct Proxy {
	OBSWeakSource scene;
	const std::function<void()> *content = nullptr; /* graphics thread, set only while the proxy renders */
};

/* Settings follow the original through its own "update" signal: libobs makes every filter private, and private
 * sources emit no global signals. */
void on_original_update(void *param, calldata_t *cd)
{
	auto *original = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	OBSDataAutoRelease settings = obs_source_get_settings(original);
	obs_source_reset_settings(static_cast<obs_source_t *>(param), settings);
}

struct FilterPair {
	FilterPair(obs_source_t *original_, obs_source_t *copy_) : original(original_), copy(copy_)
	{
		update.Connect(obs_source_get_signal_handler(original_), "update", on_original_update, copy_);
	}

	/* Strong: the signal handler must outlive the connection. A filter the user removes lingers until the next
	 * rebuild (one frame later). */
	OBSSource original;
	OBSSource copy;
	OBSSignal update; /* declared last: disconnected before either reference is dropped */

	FilterPair(FilterPair &&) noexcept = default;
	FilterPair &operator=(FilterPair &&) noexcept = default;
};

struct Mirror {
	OBSSource proxy;
	std::vector<obs_source_t *> originals; /* the scene's video filters this copy set was built from */
	std::vector<FilterPair> filters;       /* the copies that could be created */
	uint64_t last_used = 0;
	bool building = false;

	Mirror() = default;
	Mirror(Mirror &&) noexcept = default;
	Mirror &operator=(Mirror &&) noexcept = default;
};

/* Video filters of a source in enumeration order. Graphics or UI thread. */
std::vector<obs_source_t *> video_filters(obs_source_t *source)
{
	std::vector<obs_source_t *> out;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			if (obs_source_get_output_flags(filter) & OBS_SOURCE_VIDEO)
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(filter);
		},
		&out);
	return out;
}

/* Disconnects the copies and undoes the showing/active references of the proxy; the copies go with it. */
void retire(Mirror &m)
{
	m.filters.clear();
	m.originals.clear();
	if (m.proxy) {
		obs_source_dec_active(m.proxy);
		obs_source_dec_showing(m.proxy);
	}
	m.proxy = nullptr;
}

/* Mirrors dropped on the graphics thread are retired on the UI thread, where they were created. */
void retire_later(std::vector<Mirror> mirrors)
{
	if (mirrors.empty())
		return;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			std::unique_ptr<std::vector<Mirror>> list(static_cast<std::vector<Mirror> *>(param));
			for (Mirror &m : *list)
				retire(m);
		},
		new std::vector<Mirror>(std::move(mirrors)), false);
}

} // namespace

struct SceneFilters::State {
	std::mutex mutex;
	bool alive = true;
	std::unordered_map<obs_source_t *, Mirror> mirrors; /* key: scene or group source */
};

namespace {

struct BuildJob {
	std::weak_ptr<SceneFilters::State> state;
	OBSWeakSource scene;
	obs_source_t *key;
};

/* UI thread: creates the proxy and private copies of the scene's video filters, then swaps them in. */
void build(void *param)
{
	std::unique_ptr<BuildJob> job(static_cast<BuildJob *>(param));
	std::shared_ptr<SceneFilters::State> state = job->state.lock();
	if (!state)
		return;

	Mirror fresh;
	OBSSource scene = OBSGetStrongRef(job->scene);
	if (scene) {
		const std::string name = std::string("video-mixes filters: ") + obs_source_get_name(scene);
		OBSSourceAutoRelease proxy = obs_source_create_private(kProxyId, name.c_str(), nullptr);
		if (proxy) {
			static_cast<Proxy *>(obs_obj_get_data(proxy))->scene = job->scene;
			/* Insert-at-front in enumeration order reproduces the original order (see duplicate_filters). */
			fresh.originals = video_filters(scene);
			fresh.filters.reserve(fresh.originals.size());
			for (obs_source_t *original : fresh.originals) {
				OBSSourceAutoRelease copy =
					obs_source_duplicate(original, obs_source_get_name(original), true);
				if (!copy)
					continue;
				obs_source_set_enabled(copy, obs_source_enabled(original));
				obs_source_filter_add(proxy, copy);
				fresh.filters.emplace_back(original, copy.Get());
			}
			obs_source_inc_showing(proxy);
			obs_source_inc_active(proxy);
			fresh.proxy = proxy.Get();
		}
	}

	Mirror old;
	{
		std::lock_guard lock(state->mutex);
		auto it = state->mirrors.find(job->key);
		if (state->alive && it != state->mirrors.end()) {
			fresh.last_used = it->second.last_used;
			old = std::move(it->second);
			it->second = std::move(fresh);
		} else {
			old = std::move(fresh);
		}
	}
	retire(old);
}

/* ---- proxy source ---- */

uint32_t proxy_width(void *data)
{
	OBSSource scene = OBSGetStrongRef(static_cast<Proxy *>(data)->scene);
	return scene ? obs_source_get_width(scene) : 0;
}

uint32_t proxy_height(void *data)
{
	OBSSource scene = OBSGetStrongRef(static_cast<Proxy *>(data)->scene);
	return scene ? obs_source_get_height(scene) : 0;
}

enum gs_color_space proxy_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	OBSSource scene = OBSGetStrongRef(static_cast<Proxy *>(data)->scene);
	if (scene)
		return obs_source_get_color_space(scene, count, preferred);
	return count ? preferred[0] : GS_CS_SRGB;
}

void proxy_render(void *data, gs_effect_t *)
{
	if (const auto *content = static_cast<Proxy *>(data)->content)
		(*content)();
}

} // namespace

SceneFilters::SceneFilters() : state_(std::make_shared<State>()) {}

SceneFilters::~SceneFilters()
{
	std::unordered_map<obs_source_t *, Mirror> mirrors;
	{
		std::lock_guard lock(state_->mutex);
		state_->alive = false;
		mirrors.swap(state_->mirrors);
	}
	for (auto &[key, mirror] : mirrors)
		retire(mirror);
}

bool SceneFilters::render(obs_source_t *scene, const std::function<void()> &content)
{
	if (!scene || !obs_source_filter_count(scene))
		return false;
	const std::vector<obs_source_t *> current = video_filters(scene);
	if (current.empty())
		return false;

	OBSSource proxy;
	{
		std::lock_guard lock(state_->mutex);
		Mirror &m = state_->mirrors[scene];
		m.last_used = frame_;

		if (current != m.originals && !m.building) {
			m.building = true;
			obs_queue_task(OBS_TASK_UI, build, new BuildJob{state_, OBSGetWeakRef(scene), scene}, false);
		}
		if (!m.proxy)
			return false;

		/* Enabled state has no signal of its own; mirror it here. Filters added/removed meanwhile wait for the
		 * rebuild; the pairs that still exist keep working. */
		for (const FilterPair &pair : m.filters) {
			if (std::find(current.begin(), current.end(), pair.original) == current.end())
				continue;
			const bool enabled = obs_source_enabled(pair.original);
			if (obs_source_enabled(pair.copy) != enabled)
				obs_source_set_enabled(pair.copy, enabled);
		}
		proxy = m.proxy;
	}

	auto *data = static_cast<Proxy *>(obs_obj_get_data(proxy));
	data->content = &content;
	obs_source_video_render(proxy);
	data->content = nullptr;
	return true;
}

void SceneFilters::end_frame()
{
	frame_++;
	std::vector<Mirror> dropped;
	{
		std::lock_guard lock(state_->mutex);
		std::erase_if(state_->mirrors, [&](auto &entry) {
			Mirror &m = entry.second;
			if (m.building || frame_ - m.last_used <= kKeepFrames)
				return false;
			dropped.push_back(std::move(m));
			return true;
		});
	}
	retire_later(std::move(dropped));
}

void register_scene_filter_proxy()
{
	obs_source_info info = {};
	info.id = kProxyId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_SRGB;
	info.get_name = [](void *) { return "Video Mixes scene filters"; };
	info.create = [](obs_data_t *, obs_source_t *) -> void * { return new Proxy(); };
	info.destroy = [](void *data) { delete static_cast<Proxy *>(data); };
	info.get_width = proxy_width;
	info.get_height = proxy_height;
	info.video_render = proxy_render;
	info.video_get_color_space = proxy_color_space;
	obs_register_source(&info);
}

} // namespace vmix
