#include "core/visibility.hpp"

#include <obs.hpp>

#include <functional>
#include <mutex>

namespace vmix {

namespace {

constexpr const char *kKey = "video_mixes_hidden";
std::mutex g_mutex;

using ItemFn = std::function<void(obs_sceneitem_t *)>;

/* Calls fn for every item of a scene, including items inside groups. */
void for_each_item(obs_scene_t *scene, const ItemFn &fn)
{
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			const ItemFn &f = *static_cast<const ItemFn *>(param);
			f(item);
			if (obs_sceneitem_is_group(item))
				for_each_item(obs_sceneitem_group_get_scene(item), f);
			return true;
		},
		const_cast<ItemFn *>(&fn));
}

/* Calls fn for every item of every scene in the loaded scene collection. */
void for_each_item_everywhere(const ItemFn &fn)
{
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			if (obs_scene_t *scene = obs_scene_from_source(source))
				for_each_item(scene, *static_cast<const ItemFn *>(param));
			return true;
		},
		const_cast<ItemFn *>(&fn));
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
	for_each_item_everywhere([mix_bit](obs_sceneitem_t *item) { set_hidden(item, mix_bit, false); });
}

void copy_bit_everywhere(uint32_t from_bit, uint32_t to_bit)
{
	for_each_item_everywhere([from_bit, to_bit](obs_sceneitem_t *item) {
		set_hidden(item, to_bit, (hidden_mask(item) & from_bit) != 0);
	});
}

} // namespace vmix
