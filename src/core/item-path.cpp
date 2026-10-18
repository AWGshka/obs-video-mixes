#include "core/item-path.hpp"
#include "core/util.hpp"

#include <obs-frontend-api.h>

#include <algorithm>
#include <cmath>

namespace vmix {

namespace {

constexpr int kMaxDepth = 8;

std::vector<OBSSceneItem> items_of(obs_scene_t *scene)
{
	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		},
		&items);
	return items;
}

/* Depth-first, top-most items first (what the viewer sees), visible before hidden. */
bool search(obs_scene_t *scene, const std::string &uuid, bool visible_only, int depth, ItemPath &path)
{
	if (!scene || depth > kMaxDepth)
		return false;
	const std::string scene_uuid = obs_source_get_uuid(obs_scene_get_source(scene));
	auto items = items_of(scene);
	std::reverse(items.begin(), items.end());

	for (obs_sceneitem_t *item : items) {
		if (visible_only && !obs_sceneitem_visible(item))
			continue;
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (uuid == obs_source_get_uuid(src)) {
			path.push_back({scene_uuid, obs_sceneitem_get_id(item)});
			return true;
		}
	}
	for (obs_sceneitem_t *item : items) {
		if (visible_only && !obs_sceneitem_visible(item))
			continue;
		obs_scene_t *child = obs_group_or_scene_from_source(obs_sceneitem_get_source(item));
		if (!child)
			continue;
		path.push_back({scene_uuid, obs_sceneitem_get_id(item)});
		if (search(child, uuid, visible_only, depth + 1, path))
			return true;
		path.pop_back();
	}
	return false;
}

} // namespace

ItemPath find_item_path(const std::string &source_uuid)
{
	if (source_uuid.empty())
		return {};

	/* Search order: program scene, then top-level scenes, then scenes that are nested in others. A nested scene
	 * is reached through its parent first, so the placement includes the parent's transform. */
	std::vector<OBSSource> all;
	obs_enum_scenes(
		[](void *param, obs_source_t *src) {
			if (obs_source_is_scene(src))
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(src);
			return true;
		},
		&all);

	std::vector<obs_source_t *> nested;
	for (obs_source_t *scene_src : all)
		for (obs_sceneitem_t *item : items_of(obs_scene_from_source(scene_src)))
			if (obs_source_is_scene(obs_sceneitem_get_source(item)))
				nested.push_back(obs_sceneitem_get_source(item));
	auto is_nested = [&](obs_source_t *s) { return std::find(nested.begin(), nested.end(), s) != nested.end(); };

	std::vector<OBSSource> scenes;
	if (OBSSourceAutoRelease program = obs_frontend_get_current_scene())
		scenes.emplace_back(program.Get());
	for (bool want_nested : {false, true})
		for (obs_source_t *scene_src : all)
			if (is_nested(scene_src) == want_nested)
				scenes.emplace_back(scene_src);

	for (bool visible_only : {true, false}) {
		for (obs_source_t *scene_src : scenes) {
			ItemPath path;
			if (search(obs_scene_from_source(scene_src), source_uuid, visible_only, 0, path))
				return path;
		}
	}
	return {};
}

OBSDataArray item_path_to_data(const ItemPath &path)
{
	OBSDataArray arr = new_array();
	for (const ItemLink &link : path) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "scene", link.scene_uuid.c_str());
		obs_data_set_int(d, "item", link.item_id);
		obs_data_array_push_back(arr, d);
	}
	return arr;
}

ItemPath item_path_from_data(obs_data_array_t *data)
{
	ItemPath path;
	for (size_t i = 0, n = obs_data_array_count(data); i < n; i++) {
		OBSDataAutoRelease d = obs_data_array_item(data, i);
		path.push_back({obs_data_get_string(d, "scene"), obs_data_get_int(d, "item")});
	}
	return path;
}

uint32_t PlacedItem::width() const
{
	return (uint32_t)std::lround(std::max(0.0f, max.x - min.x));
}

uint32_t PlacedItem::height() const
{
	return (uint32_t)std::lround(std::max(0.0f, max.y - min.y));
}

bool place_item(const ItemPath &path, PlacedItem &out)
{
	if (path.empty())
		return false;

	/* parent = D_inner * ... * D_outer, matching the order libobs' scene rendering multiplies transforms. */
	matrix4_identity(&out.parent);
	OBSSceneItem item;
	for (size_t i = 0; i < path.size(); i++) {
		OBSSourceAutoRelease scene_src = obs_get_source_by_uuid(path[i].scene_uuid.c_str());
		obs_scene_t *scene = scene_src ? obs_group_or_scene_from_source(scene_src) : nullptr;
		obs_sceneitem_t *found = scene ? obs_scene_find_sceneitem_by_id(scene, path[i].item_id) : nullptr;
		if (!found)
			return false;
		item = found;
		if (i + 1 < path.size()) {
			matrix4 draw;
			obs_sceneitem_get_draw_transform(found, &draw);
			matrix4_mul(&out.parent, &draw, &out.parent);
		}
	}

	/* Bounding box of the item's box (unit square → canvas) carried through the parent chain. */
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	out.min = {{{INFINITY, INFINITY}}};
	out.max = {{{-INFINITY, -INFINITY}}};
	for (float cx : {0.0f, 1.0f}) {
		for (float cy : {0.0f, 1.0f}) {
			vec3 p;
			vec3_set(&p, cx, cy, 0.0f);
			vec3_transform(&p, &p, &box);
			vec3_transform(&p, &p, &out.parent);
			out.min.x = std::min(out.min.x, p.x);
			out.min.y = std::min(out.min.y, p.y);
			out.max.x = std::max(out.max.x, p.x);
			out.max.y = std::max(out.max.y, p.y);
		}
	}
	out.item = item;
	return out.width() > 0 && out.height() > 0;
}

} // namespace vmix
