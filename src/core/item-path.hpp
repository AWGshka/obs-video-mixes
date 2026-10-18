#pragma once

#include <obs.hpp>
#include <graphics/matrix4.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vmix {

/* One step from a scene (or group) to an item in it. A path runs from a top-level scene down to the item that
 * shows the source, through any nested scenes / groups in between. */
struct ItemLink {
	std::string scene_uuid;
	int64_t item_id = 0;
};
using ItemPath = std::vector<ItemLink>;

/*
 * Where a source is shown: the current program scene first (including nested scenes and groups), then any other
 * scene. Visible placements win over hidden ones. Empty if the source is not placed in any scene. UI thread.
 */
ItemPath find_item_path(const std::string &source_uuid);

OBSDataArray item_path_to_data(const ItemPath &path);
ItemPath item_path_from_data(obs_data_array_t *data);

/* A resolved placement: the item, the transform of its parent chain, and its bounding box on the program canvas
 * (crop, scale, bounds, rotation and nesting applied). */
struct PlacedItem {
	OBSSceneItem item;
	matrix4 parent;
	vec2 min;
	vec2 max;

	uint32_t width() const;
	uint32_t height() const;
};

/* Resolves a path against the live scenes; false if any step no longer exists. Any thread. */
bool place_item(const ItemPath &path, PlacedItem &out);

} // namespace vmix
