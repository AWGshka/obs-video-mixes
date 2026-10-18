#include "render/root-source.hpp"
#include "core/item-path.hpp"
#include "render/mix-renderer.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <memory>

namespace vmix {

namespace {

struct RootSource {
	uint32_t mix_id = 0;
	uint32_t cx = 0;
	uint32_t cy = 0;
	std::atomic<bool> fade{true};
	bool source_mode = false;
	OBSWeakSource target;                  /* source mixes: the recorded source */
	ItemPath placement;                    /* source mixes: where the source sits in a scene (may be empty) */
	std::unique_ptr<MixRenderer> renderer; /* graphics thread only */
};

void *root_create(obs_data_t *settings, obs_source_t *)
{
	auto *root = new RootSource();
	root->mix_id = (uint32_t)obs_data_get_int(settings, "mix_id");
	root->cx = (uint32_t)obs_data_get_int(settings, "width");
	root->cy = (uint32_t)obs_data_get_int(settings, "height");
	root->fade = obs_data_get_bool(settings, "fade");
	if (const char *uuid = obs_data_get_string(settings, "source_uuid"); uuid && *uuid) {
		root->source_mode = true;
		OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid);
		if (src)
			root->target = OBSGetWeakRef(src);
		OBSDataArrayAutoRelease path = obs_data_get_array(settings, "item_path");
		root->placement = item_path_from_data(path);
	}
	return root;
}

void root_destroy(void *data)
{
	auto *root = static_cast<RootSource *>(data);
	if (root->renderer) {
		obs_enter_graphics();
		root->renderer.reset();
		obs_leave_graphics();
	}
	delete root;
}

void root_update(void *data, obs_data_t *settings)
{
	static_cast<RootSource *>(data)->fade = obs_data_get_bool(settings, "fade");
}

uint32_t root_width(void *data)
{
	return static_cast<RootSource *>(data)->cx;
}

uint32_t root_height(void *data)
{
	return static_cast<RootSource *>(data)->cy;
}

/*
 * Source mix: the source as it appears in its scene (crop, scale, bounds, rotation, nesting), cut out at its
 * on-canvas bounding box. If the placement changes size mid-recording it is letterboxed into the canvas.
 */
bool render_placed(RootSource *root)
{
	PlacedItem placed;
	if (root->placement.empty() || !place_item(root->placement, placed))
		return false;

	const float w = placed.max.x - placed.min.x;
	const float h = placed.max.y - placed.min.y;
	const float scale = std::min((float)root->cx / w, (float)root->cy / h);
	if (!root->renderer)
		root->renderer = std::make_unique<MixRenderer>(root->mix_id);

	/* Pre-multiplied stack: the last call applies first (item transform → parents → crop to box → fit). */
	gs_matrix_push();
	gs_matrix_translate3f(((float)root->cx - w * scale) * 0.5f, ((float)root->cy - h * scale) * 0.5f, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	gs_matrix_translate3f(-placed.min.x, -placed.min.y, 0.0f);
	gs_matrix_mul(&placed.parent);
	root->renderer->render_item(placed.item);
	gs_matrix_pop();
	return true;
}

/* Source mix fallback when the source is not placed in any scene: the source alone at its own size. */
void render_source(RootSource *root)
{
	OBSSource src = OBSGetStrongRef(root->target);
	if (!src)
		return;
	const uint32_t w = obs_source_get_width(src);
	const uint32_t h = obs_source_get_height(src);
	if (!w || !h)
		return;

	const float scale = std::min((float)root->cx / (float)w, (float)root->cy / (float)h);
	gs_matrix_push();
	gs_matrix_translate3f(((float)root->cx - (float)w * scale) * 0.5f, ((float)root->cy - (float)h * scale) * 0.5f,
			      0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	obs_source_video_render(src);
	gs_matrix_pop();
}

void root_render(void *data, gs_effect_t *)
{
	auto *root = static_cast<RootSource *>(data);
	if (!root->mix_id || !root->cx || !root->cy)
		return;
	if (root->source_mode) {
		if (!render_placed(root))
			render_source(root);
		return;
	}
	if (!root->renderer)
		root->renderer = std::make_unique<MixRenderer>(root->mix_id);
	root->renderer->set_fade(root->fade.load(std::memory_order_relaxed));
	root->renderer->render(root->cx, root->cy);
}

} // namespace

void register_root_source()
{
	obs_source_info info = {};
	info.id = kRootSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_SRGB;
	info.get_name = [](void *) { return obs_module_text("RootSource"); };
	info.create = root_create;
	info.destroy = root_destroy;
	info.update = root_update;
	info.get_width = root_width;
	info.get_height = root_height;
	info.video_render = root_render;
	obs_register_source(&info);
}

} // namespace vmix
