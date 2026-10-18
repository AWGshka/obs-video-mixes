#include "render/geometry.hpp"

#include <cmath>

namespace vmix {

namespace {

uint32_t cropped(uint32_t size, int a, int b)
{
	const uint32_t c = (uint32_t)(a + b);
	return c > size ? 2 : size - c;
}

} // namespace

ItemGeometry item_geometry(obs_sceneitem_t *item, uint32_t source_cx, uint32_t source_cy)
{
	ItemGeometry g{};
	obs_sceneitem_get_crop(item, &g.crop);

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);

	vec2 scale = info.scale;
	const uint32_t cx = cropped(source_cx, g.crop.left, g.crop.right);
	const uint32_t cy = cropped(source_cy, g.crop.top, g.crop.bottom);

	if (info.bounds_type != OBS_BOUNDS_NONE && cx && cy && info.bounds.x > 0.0f && info.bounds.y > 0.0f) {
		const vec2 bounds = info.bounds;
		float width = (float)cx * std::fabs(scale.x);
		float height = (float)cy * std::fabs(scale.y);
		const float item_aspect = width / height;
		const float bounds_aspect = bounds.x / bounds.y;
		uint32_t type = info.bounds_type;

		if (type == OBS_BOUNDS_MAX_ONLY && (width > bounds.x || height > bounds.y))
			type = OBS_BOUNDS_SCALE_INNER;

		if (type == OBS_BOUNDS_SCALE_INNER || type == OBS_BOUNDS_SCALE_OUTER) {
			bool use_width = bounds_aspect < item_aspect;
			if (info.bounds_type == OBS_BOUNDS_SCALE_OUTER)
				use_width = !use_width;
			vec2_mulf(&scale, &scale, use_width ? bounds.x / width : bounds.y / height);
		} else if (type == OBS_BOUNDS_SCALE_TO_WIDTH) {
			vec2_mulf(&scale, &scale, bounds.x / width);
		} else if (type == OBS_BOUNDS_SCALE_TO_HEIGHT) {
			vec2_mulf(&scale, &scale, bounds.y / height);
		} else if (type == OBS_BOUNDS_STRETCH) {
			scale.x = std::copysign(bounds.x / (float)cx, scale.x);
			scale.y = std::copysign(bounds.y / (float)cy, scale.y);
		}

		width = (float)cx * scale.x;
		height = (float)cy * scale.y;
		const float width_diff = bounds.x - std::fabs(width);
		const float height_diff = bounds.y - std::fabs(height);

		const bool crop_to_bounds = info.crop_to_bounds &&
					    (info.bounds_type == OBS_BOUNDS_SCALE_OUTER ||
					     info.bounds_type == OBS_BOUNDS_SCALE_TO_HEIGHT ||
					     info.bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH);

		if (crop_to_bounds && (width_diff < -0.1f || height_diff < -0.1f)) {
			const bool crop_width = width_diff < -0.1f;
			const bool flipped = crop_width ? width < 0.0f : height < 0.0f;
			const float diff = crop_width ? width_diff : height_diff;
			const float axis_scale = crop_width ? scale.x : scale.y;
			const uint32_t mask = crop_width ? (OBS_ALIGN_LEFT | OBS_ALIGN_RIGHT)
							 : (OBS_ALIGN_TOP | OBS_ALIGN_BOTTOM);
			const uint32_t align = info.bounds_alignment & mask;

			const float overdraw = std::fabs(diff / axis_scale);
			float tl;
			if (align & (OBS_ALIGN_TOP | OBS_ALIGN_LEFT))
				tl = 0.0f;
			else if (align & (OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT))
				tl = overdraw;
			else
				tl = overdraw / 2.0f;
			const float br = overdraw - tl;

			/* Crops are swapped if the axis is flipped. */
			const int crop_tl = (int)std::round(flipped ? br : tl);
			const int crop_br = (int)std::round(flipped ? tl : br);

			if (crop_width) {
				g.bounds_crop.left = crop_tl;
				g.bounds_crop.right = crop_br;
			} else {
				g.bounds_crop.top = crop_tl;
				g.bounds_crop.bottom = crop_br;
			}
		}
	}

	g.output_scale = scale;
	return g;
}

} // namespace vmix
