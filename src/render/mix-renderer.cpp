#include "render/mix-renderer.hpp"

#include "core/visibility.hpp"
#include "render/geometry.hpp"

#include <graphics/math-defs.h>
#include <graphics/matrix4.h>
#include <obs-module.h>
#include <util/util.hpp>

#include <cmath>

namespace vmix {

namespace {

constexpr int kMaxDepth = 16;

/* Copy of obs_blend_mode_params in libobs/obs-scene.c (indexed by enum obs_blending_type). */
struct BlendParams {
	gs_blend_type src_color, src_alpha, dst_color, dst_alpha;
	gs_blend_op_type op;
};
constexpr BlendParams kBlend[] = {
	/* NORMAL   */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	/* ADDITIVE */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_ADD},
	/* SUBTRACT */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_REVERSE_SUBTRACT},
	/* SCREEN   */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	/* MULTIPLY */ {GS_BLEND_DSTCOLOR, GS_BLEND_DSTALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
	/* LIGHTEN  */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MAX},
	/* DARKEN   */ {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MIN},
};

obs_scene_t *scene_of(obs_source_t *source)
{
	obs_scene_t *scene = obs_scene_from_source(source);
	return scene ? scene : obs_group_from_source(source);
}

uint32_t cropped(uint32_t size, int a, int b)
{
	const uint32_t c = (uint32_t)(a + b);
	return c > size ? 2 : size - c;
}

/* Technique selection from render_item_texture() in libobs/obs-scene.c. */
const char *draw_technique(bool upscale, enum gs_color_space current, enum gs_color_space source)
{
	const bool cur_sdr = current == GS_CS_SRGB || current == GS_CS_SRGB_16F;
	switch (source) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current == GS_CS_709_SCRGB)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	case GS_CS_709_EXTENDED:
		if (cur_sdr)
			return upscale ? "DrawUpscaleTonemap" : "DrawTonemap";
		if (current == GS_CS_709_SCRGB)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	case GS_CS_709_SCRGB:
		if (cur_sdr)
			return upscale ? "DrawUpscaleMultiplyTonemap" : "DrawMultiplyTonemap";
		if (current == GS_CS_709_EXTENDED)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	}
	return upscale ? "DrawUpscale" : "Draw";
}

/*
 * libobs' transition_active() reads private state. A scene item's show/hide transition is added as an active child
 * of its scene for exactly as long as it plays (obs_sceneitem_do_transition / obs_sceneitem_transition_stop), so for
 * the program (active) scenes a mix renders, "active" is the same thing. Item transitions are private sources, which
 * emit no global signals, so this cannot be tracked through signals instead.
 */
bool transition_active(obs_source_t *transition)
{
	return transition && obs_source_active(transition);
}

float sdr_multiplier(enum gs_color_space current, enum gs_color_space source)
{
	const bool src_sdr = source == GS_CS_SRGB || source == GS_CS_SRGB_16F || source == GS_CS_709_EXTENDED;
	const bool cur_sdr = current == GS_CS_SRGB || current == GS_CS_SRGB_16F || current == GS_CS_709_EXTENDED;
	if (current == GS_CS_709_SCRGB && src_sdr)
		return obs_get_video_sdr_white_level() / 80.0f;
	if (source == GS_CS_709_SCRGB && cur_sdr)
		return 80.0f / obs_get_video_sdr_white_level();
	return 1.0f;
}

} // namespace

MixRenderer::MixRenderer(uint32_t mix_id) : bit_(1u << mix_id)
{
	scratch_.resize(kMaxDepth + 1);
}

MixRenderer::~MixRenderer()
{
	for (auto &[item, target] : targets_)
		gs_texrender_destroy(target.tex);
	gs_texrender_destroy(fade_tex_[0]);
	gs_texrender_destroy(fade_tex_[1]);
	gs_effect_destroy(fade_effect_);
	gs_samplerstate_destroy(point_sampler_);
}

void MixRenderer::render(uint32_t cx, uint32_t cy)
{
	frame_++;

	for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
		obs_source_t *source = obs_get_output_source(i);
		if (!source)
			continue;
		Channel ch{source};
		if (obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
			ch.transition = true;
			ch.a = obs_transition_get_source(source, OBS_TRANSITION_SOURCE_A);
			ch.b = obs_transition_get_source(source, OBS_TRANSITION_SOURCE_B);
		}
		channels_.push_back(ch);
	}

	bool any_hidden = false;
	for (const Channel &ch : channels_) {
		if (ch.transition) {
			visit(ch.a, 0);
			visit(ch.b, 0);
			any_hidden = any_hidden || hidden_below(ch.a) || hidden_below(ch.b);
		} else {
			visit(ch.source, 0);
			any_hidden = any_hidden || hidden_below(ch.source);
		}
	}

	if (!any_hidden) {
		/* Nothing differs from program: reuse the finished main texture. */
		obs_render_main_texture();
	} else {
		for (const Channel &ch : channels_) {
			if (ch.transition)
				draw_transition(ch, cx, cy);
			else
				draw_source(ch.source);
		}
	}

	sweep_targets();
	filters_.end_frame();
	release_frame();
}

void MixRenderer::render_item(obs_sceneitem_t *item)
{
	frame_++;
	const Item it{item, obs_sceneitem_get_source(item), 0, true, -1};
	gs_blend_state_push();
	gs_reset_blend_state();
	draw_item(it);
	gs_blend_state_pop();
	sweep_targets();
}

int MixRenderer::visit(obs_source_t *source, int depth)
{
	if (!source || depth > kMaxDepth)
		return -1;
	obs_scene_t *scene = scene_of(source);
	if (!scene)
		return -1;
	if (auto it = node_index_.find(source); it != node_index_.end())
		return it->second;

	/* Reserve the node first so a (theoretical) cycle terminates. */
	const int index = (int)nodes_.size();
	nodes_.push_back({source, 0, 0, false});
	node_index_.emplace(source, index);

	std::vector<Item> &local = scratch_[depth];
	local.clear();
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<Item> *>(param)->push_back({item, obs_sceneitem_get_source(item),
									    hidden_mask(item),
									    obs_sceneitem_visible(item), -1});
			return true;
		},
		&local);

	bool has_hidden = false;
	for (Item &item : local) {
		item.child = visit(item.source, depth + 1);
		if (!item.visible)
			continue;
		if ((item.mask & bit_) || (item.child >= 0 && nodes_[item.child].has_hidden))
			has_hidden = true;
	}

	Node &node = nodes_[index];
	node.first = (uint32_t)items_.size();
	node.count = (uint32_t)local.size();
	node.has_hidden = has_hidden;
	items_.insert(items_.end(), local.begin(), local.end());
	return index;
}

bool MixRenderer::hidden_below(obs_source_t *source) const
{
	if (!source)
		return false;
	auto it = node_index_.find(source);
	return it != node_index_.end() && nodes_[it->second].has_hidden;
}

void MixRenderer::release_frame()
{
	for (const Item &item : items_)
		obs_sceneitem_release(item.item);
	for (const Channel &ch : channels_) {
		obs_source_release(ch.a);
		obs_source_release(ch.b);
		obs_source_release(ch.source);
	}
	items_.clear();
	nodes_.clear();
	channels_.clear();
	node_index_.clear();
}

void MixRenderer::draw_source(obs_source_t *source)
{
	if (!source)
		return;
	auto it = node_index_.find(source);
	if (it == node_index_.end() || !nodes_[it->second].has_hidden)
		obs_source_video_render(source);
	else
		draw_scene(nodes_[it->second]);
}

void MixRenderer::draw_scene(const Node &node)
{
	if (!filters_.render(node.source, [&] { draw_node(node); }))
		draw_node(node);
}

/* Mirrors scene_video_render(): items bottom to top, skipping the ones hidden in this mix. Items that are
 * being hidden with a hide transition stay until it ends, as in libobs. */
void MixRenderer::draw_node(const Node &node)
{
	gs_blend_state_push();
	gs_reset_blend_state();
	for (uint32_t i = node.first, end = node.first + node.count; i < end; i++) {
		const Item &item = items_[i];
		if (item.mask & bit_)
			continue;
		if (item.visible || transition_active(obs_sceneitem_get_transition(item.item, false)))
			draw_item(item);
	}
	gs_blend_state_pop();
}

/* The item's own content, or its show/hide transition while one plays (render_item in libobs/obs-scene.c). */
void MixRenderer::draw_content(const Item &item)
{
	const bool rerender = item.child >= 0 && nodes_[item.child].has_hidden;

	/* A transition renders its child through libobs, which would bring hidden items back: a re-rendered
	 * subtree skips the animation instead. */
	obs_source_t *transition = rerender ? nullptr : obs_sceneitem_get_transition(item.item, item.visible);
	if (transition_active(transition)) {
		obs_transition_set_size(transition, obs_source_get_width(item.source), obs_source_get_height(item.source));
		obs_source_video_render(transition);
	} else if (rerender) {
		draw_scene(nodes_[item.child]);
	} else {
		obs_source_video_render(item.source);
	}
}

/* Mirrors render_item() in libobs/obs-scene.c, using renderer-owned render targets. */
void MixRenderer::draw_item(const Item &item)
{
	obs_source_t *source = item.source;
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	const ItemGeometry geo = item_geometry(item.item, width, height);

	const obs_scale_type scale_filter = obs_sceneitem_get_scale_filter(item.item);
	const obs_blending_method blend_method = obs_sceneitem_get_blending_method(item.item);
	const obs_blending_type blend_type = obs_sceneitem_get_blending_mode(item.item);
	const bool nested_scene = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(source);

	const bool use_texture = crop_enabled(geo.crop) || crop_enabled(geo.bounds_crop) ||
				 scale_filter != OBS_SCALE_DISABLE || blend_method == OBS_BLEND_METHOD_SRGB_OFF ||
				 blend_type != OBS_BLEND_NORMAL || nested_scene;

	const enum gs_color_space current_space = gs_get_color_space();
	const enum gs_color_space source_space = obs_source_get_color_space(source, 1, &current_space);

	gs_texrender_t *target = nullptr;
	if (use_texture) {
		if (!width || !height)
			return;
		target = item_target(item.item, gs_get_format_from_space(source_space));

		const auto &c = geo.crop;
		const auto &b = geo.bounds_crop;
		const uint32_t cx = cropped(width, c.left + b.left, c.right + b.right);
		const uint32_t cy = cropped(height, c.top + b.top, c.bottom + b.bottom);

		if (cx && cy && gs_texrender_begin_with_color_space(target, cx, cy, source_space)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
			gs_matrix_scale3f((float)width / (float)cx, (float)height / (float)cy, 1.0f);
			gs_matrix_translate3f(-(float)(c.left + b.left), -(float)(c.top + b.top), 0.0f);
			draw_content(item);
			gs_texrender_end(target);
		}
	}

	const bool linear_srgb = !target || blend_method != OBS_BLEND_METHOD_SRGB_OFF;
	const bool previous = gs_set_linear_srgb(linear_srgb);

	matrix4 transform;
	obs_sceneitem_get_draw_transform(item.item, &transform);
	gs_matrix_push();
	gs_matrix_mul(&transform);
	if (target) {
		if (gs_texture_t *tex = gs_texrender_get_texture(target))
			draw_item_texture(tex, item.item, geo.output_scale, current_space, source_space);
	} else {
		draw_content(item);
	}
	gs_matrix_pop();

	gs_set_linear_srgb(previous);
}

/* Mirrors render_item_texture() in libobs/obs-scene.c. */
void MixRenderer::draw_item_texture(gs_texture_t *tex, obs_sceneitem_t *item, const vec2 &output_scale,
				    enum gs_color_space current_space, enum gs_color_space source_space)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const obs_scale_type type = obs_sceneitem_get_scale_filter(item);
	const uint32_t cx = gs_texture_get_width(tex);
	const uint32_t cy = gs_texture_get_height(tex);
	bool upscale = false;

	if (type == OBS_SCALE_POINT) {
		if (!point_sampler_) {
			gs_sampler_info info = {};
			info.filter = GS_FILTER_POINT;
			info.address_u = GS_ADDRESS_CLAMP;
			info.address_v = GS_ADDRESS_CLAMP;
			info.address_w = GS_ADDRESS_CLAMP;
			info.max_anisotropy = 1;
			point_sampler_ = gs_samplerstate_create(&info);
		}
		gs_effect_set_next_sampler(gs_effect_get_param_by_name(effect, "image"), point_sampler_);
	} else if (type != OBS_SCALE_DISABLE &&
		   (!close_float(output_scale.x, 1.0f, EPSILON) || !close_float(output_scale.y, 1.0f, EPSILON))) {
		if (output_scale.x < 0.5f || output_scale.y < 0.5f) {
			effect = obs_get_base_effect(OBS_EFFECT_BILINEAR_LOWRES);
		} else if (type == OBS_SCALE_BICUBIC) {
			effect = obs_get_base_effect(OBS_EFFECT_BICUBIC);
		} else if (type == OBS_SCALE_LANCZOS) {
			effect = obs_get_base_effect(OBS_EFFECT_LANCZOS);
		} else if (type == OBS_SCALE_AREA) {
			effect = obs_get_base_effect(OBS_EFFECT_AREA);
			upscale = output_scale.x >= 1.0f && output_scale.y >= 1.0f;
		}

		if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "base_dimension")) {
			const vec2 base = {{{(float)cx, (float)cy}}};
			gs_effect_set_vec2(p, &base);
		}
		if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "base_dimension_i")) {
			const vec2 base_i = {{{1.0f / (float)cx, 1.0f / (float)cy}}};
			gs_effect_set_vec2(p, &base_i);
		}
	}

	if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "multiplier"))
		gs_effect_set_float(p, sdr_multiplier(current_space, source_space));

	const obs_blending_type blend_type = obs_sceneitem_get_blending_mode(item);
	const BlendParams &bp = kBlend[(size_t)blend_type < std::size(kBlend) ? blend_type : OBS_BLEND_NORMAL];

	gs_blend_state_push();
	gs_blend_function_separate(bp.src_color, bp.dst_color, bp.src_alpha, bp.dst_alpha);
	gs_blend_op(bp.op);
	const char *tech = draw_technique(upscale, current_space, source_space);
	while (gs_effect_loop(effect, tech))
		obs_source_draw(tex, 0, 0, 0, 0, false);
	gs_blend_state_pop();
}

void MixRenderer::draw_transition(const Channel &ch, uint32_t cx, uint32_t cy)
{
	if (!hidden_below(ch.a) && !hidden_below(ch.b)) {
		/* Neither side differs from program: keep the exact transition (stingers, luma wipes...). */
		obs_source_video_render(ch.source);
		return;
	}
	if (!ch.b) {
		draw_source(ch.a);
		return;
	}

	const float t = obs_transition_get_time(ch.source);
	if (!ch.a || !fade_.load(std::memory_order_relaxed) || fade_effect_failed_) {
		draw_source(t < 0.5f ? ch.a : ch.b);
		return;
	}
	draw_fade(ch.a, ch.b, t, cx, cy);
}

void MixRenderer::draw_fade(obs_source_t *a, obs_source_t *b, float t, uint32_t cx, uint32_t cy)
{
	if (!fade_effect_) {
		BPtr<char> path = obs_module_file("effects/mix-fade.effect");
		fade_effect_ = path ? gs_effect_create_from_file(path, nullptr) : nullptr;
		if (!fade_effect_) {
			blog(LOG_WARNING, "[video-mixes] Failed to load mix-fade.effect, falling back to cuts");
			fade_effect_failed_ = true;
			draw_source(t < 0.5f ? a : b);
			return;
		}
	}

	const enum gs_color_space space = gs_get_color_space();
	const enum gs_color_format format = gs_get_format_from_space(space);
	obs_source_t *sides[2] = {a, b};

	const bool previous_linear = gs_set_linear_srgb(true);
	for (int i = 0; i < 2; i++) {
		gs_texrender_t *&tex = fade_tex_[i];
		if (tex && gs_texrender_get_format(tex) != format) {
			gs_texrender_destroy(tex);
			tex = nullptr;
		}
		if (!tex)
			tex = gs_texrender_create(format, GS_ZS_NONE);
		gs_texrender_reset(tex);
		if (gs_texrender_begin_with_color_space(tex, cx, cy, space)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			draw_source(sides[i]);
			gs_texrender_end(tex);
		}
	}

	gs_eparam_t *pa = gs_effect_get_param_by_name(fade_effect_, "tex_a");
	gs_eparam_t *pb = gs_effect_get_param_by_name(fade_effect_, "tex_b");
	gs_eparam_t *pt = gs_effect_get_param_by_name(fade_effect_, "fade_val");
	gs_texture_t *ta = gs_texrender_get_texture(fade_tex_[0]);
	gs_texture_t *tb = gs_texrender_get_texture(fade_tex_[1]);

	/* Same color handling as OBS' fade transition (transition-fade.c). */
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	const char *tech = "Fade";
	if (space == GS_CS_SRGB) {
		gs_effect_set_texture(pa, ta);
		gs_effect_set_texture(pb, tb);
	} else {
		gs_effect_set_texture_srgb(pa, ta);
		gs_effect_set_texture_srgb(pb, tb);
		tech = "FadeLinear";
	}
	gs_effect_set_float(pt, t);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(fade_effect_, tech))
		gs_draw_sprite(nullptr, 0, cx, cy);
	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous_srgb);
	gs_set_linear_srgb(previous_linear);
}

gs_texrender_t *MixRenderer::item_target(const obs_sceneitem_t *item, enum gs_color_format format)
{
	Target &t = targets_[item];
	if (t.tex && gs_texrender_get_format(t.tex) != format) {
		gs_texrender_destroy(t.tex);
		t.tex = nullptr;
	}
	if (!t.tex)
		t.tex = gs_texrender_create(format, GS_ZS_NONE);
	if (t.frame != frame_) {
		/* An item drawn twice in one frame (nested scene used twice) reuses its first render. */
		gs_texrender_reset(t.tex);
		t.frame = frame_;
	}
	return t.tex;
}

void MixRenderer::sweep_targets()
{
	std::erase_if(targets_, [&](const auto &entry) {
		if (entry.second.frame == frame_)
			return false;
		gs_texrender_destroy(entry.second.tex);
		return true;
	});
}

} // namespace vmix
