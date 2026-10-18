#include "core/mix-canvas.hpp"
#include "core/item-path.hpp"
#include "render/root-source.hpp"

#include <obs-module.h>

namespace vmix {

namespace {

std::string format1(const char *key, const std::string &arg)
{
	std::string s = obs_module_text(key);
	if (auto pos = s.find("%1"); pos != std::string::npos)
		s.replace(pos, 2, arg);
	return s;
}

uint32_t even(uint32_t v)
{
	return std::max(2u, v & ~1u);
}

} // namespace

std::unique_ptr<MixCanvas> MixCanvas::create(const MixConfig &cfg, std::string &error)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		error = obs_module_text("Error.Canvas");
		return nullptr;
	}

	const bool explicit_size = cfg.width >= 2 && cfg.height >= 2;
	OBSSourceAutoRelease target;
	ItemPath placement;

	if (cfg.kind == MixKind::Source) {
		target = obs_get_source_by_uuid(cfg.source_uuid.c_str());
		if (!target) {
			error = format1("Error.SourceMissing", cfg.source_name);
			return nullptr;
		}
		/* The size the source occupies on screen (crop/scale/bounds applied); its own size if not placed. */
		uint32_t cx = obs_source_get_width(target);
		uint32_t cy = obs_source_get_height(target);
		placement = find_item_path(cfg.source_uuid);
		if (PlacedItem placed; place_item(placement, placed)) {
			cx = placed.width();
			cy = placed.height();
		} else {
			placement.clear();
		}
		if (explicit_size) {
			cx = cfg.width;
			cy = cfg.height;
		}
		if (!cx || !cy) {
			error = format1("Error.SourceNoVideo", obs_source_get_name(target));
			return nullptr;
		}
		/* The whole canvas is the source: no scene layout, no black surroundings. */
		ovi.base_width = ovi.output_width = even(cx);
		ovi.base_height = ovi.output_height = even(cy);
	} else if (explicit_size) {
		/* Base must match program so scene coordinates line up; only the output scales. */
		ovi.output_width = even(cfg.width);
		ovi.output_height = even(cfg.height);
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "mix_id", cfg.id);
	obs_data_set_int(settings, "width", ovi.base_width);
	obs_data_set_int(settings, "height", ovi.base_height);
	obs_data_set_bool(settings, "fade", cfg.transitions == TransitionStyle::Fade);
	if (target) {
		obs_data_set_string(settings, "source_uuid", cfg.source_uuid.c_str());
		obs_data_set_array(settings, "item_path", item_path_to_data(placement));
	}

	const std::string name = "Video Mix: " + cfg.name;
	OBSSourceAutoRelease root = obs_source_create_private(kRootSourceId, name.c_str(), settings);
	OBSCanvasAutoRelease canvas = obs_canvas_create_private(name.c_str(), &ovi, EPHEMERAL);
	if (!root || !canvas || !obs_canvas_has_video(canvas)) {
		blog(LOG_ERROR, "[video-mixes] Failed to create canvas for mix '%s'", cfg.name.c_str());
		if (canvas)
			obs_canvas_remove(canvas);
		error = obs_module_text("Error.Canvas");
		return nullptr;
	}

	std::unique_ptr<MixCanvas> mc(new MixCanvas());
	if (target) {
		obs_source_inc_showing(target);
		obs_source_inc_active(target);
		mc->target_ = target.Get();
	}
	obs_canvas_set_channel(canvas, 0, root);
	mc->root_ = root.Get();
	mc->canvas_ = canvas.Get();

	blog(LOG_INFO, "[video-mixes] Mix '%s' canvas created (%ux%u → %ux%u)", cfg.name.c_str(), ovi.base_width,
	     ovi.base_height, ovi.output_width, ovi.output_height);
	return mc;
}

MixCanvas::~MixCanvas()
{
	if (canvas_) {
		obs_canvas_set_channel(canvas_, 0, nullptr);
		obs_canvas_remove(canvas_);
	}
	if (target_) {
		obs_source_dec_active(target_);
		obs_source_dec_showing(target_);
	}
}

void MixCanvas::set_fade(bool fade)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "fade", fade);
	obs_source_update(root_, settings);
}

} // namespace vmix
