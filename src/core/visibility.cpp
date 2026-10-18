#include "core/visibility.hpp"

#include <obs.hpp>

#include <mutex>

namespace vmix {

namespace {

constexpr const char *kKey = "video_mixes_hidden";
std::mutex g_mutex;

void clear_in_scene(obs_scene_t *scene, uint32_t bit)
{
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			const uint32_t b = *static_cast<uint32_t *>(param);
			set_hidden(item, b, false);
			if (obs_sceneitem_is_group(item))
				clear_in_scene(obs_sceneitem_group_get_scene(item), b);
			return true;
		},
		&bit);
}

} // namespace

uint32_t hidden_mask(obs_sceneitem_t *item)
{
	OBSDataAutoRelease data = obs_sceneitem_get_private_settings(item);
	if (!data)
		return 0;
	std::lock_guard lock(g_mutex);
	return (uint32_t)obs_data_get_int(data, kKey);
}

void set_hidden(obs_sceneitem_t *item, uint32_t mix_bit, bool hidden)
{
	OBSDataAutoRelease data = obs_sceneitem_get_private_settings(item);
	if (!data)
		return;
	std::lock_guard lock(g_mutex);
	const uint32_t cur = (uint32_t)obs_data_get_int(data, kKey);
	const uint32_t next = hidden ? (cur | mix_bit) : (cur & ~mix_bit);
	if (next == cur)
		return;
	if (next)
		obs_data_set_int(data, kKey, next);
	else
		obs_data_erase(data, kKey);
}

void clear_bit_everywhere(uint32_t mix_bit)
{
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			if (obs_scene_t *scene = obs_scene_from_source(source))
				clear_in_scene(scene, *static_cast<uint32_t *>(param));
			return true;
		},
		&mix_bit);
}

} // namespace vmix
