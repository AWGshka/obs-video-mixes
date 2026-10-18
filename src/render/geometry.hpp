#pragma once

#include <obs.h>

namespace vmix {

struct ItemGeometry {
	obs_sceneitem_crop crop;        /* user crop */
	obs_sceneitem_crop bounds_crop; /* crop-to-bounds overdraw */
	vec2 output_scale;              /* effective scale incl. bounds, selects the scale shader */
};

/*
 * Recomputes the parts of libobs' private item transform state that are not exported
 * (bounds_crop / output_scale), mirroring update_item_transform() + calculate_bounds_data()
 * in libobs/obs-scene.c (OBS 32.x). Uses absolute values from obs_sceneitem_get_info2().
 */
ItemGeometry item_geometry(obs_sceneitem_t *item, uint32_t source_cx, uint32_t source_cy);

inline bool crop_enabled(const obs_sceneitem_crop &c)
{
	return c.left || c.right || c.top || c.bottom;
}

} // namespace vmix
