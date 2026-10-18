#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vmix {

/*
 * Renders the program output with the scene items hidden for one mix removed.
 *
 * libobs caches scene item / nested scene / transition renders once per frame (reset in the tick), so a
 * second render of the same scene graph in the same frame would reuse textures that still contain the
 * hidden items. This renderer therefore walks the scene graph itself for every subtree that contains a
 * hidden item, using its own per-item render targets and the item's live transform, crop, scale filter
 * and blend settings. Subtrees without hidden items are drawn through libobs (sharing its caches), and if
 * nothing is hidden at all the finished main texture is reused, so an unused mix costs a single blit.
 *
 * All methods except set_fade() must be called on the graphics thread inside a graphics context.
 */
class MixRenderer {
public:
	explicit MixRenderer(uint32_t mix_id);
	~MixRenderer();

	MixRenderer(const MixRenderer &) = delete;
	MixRenderer &operator=(const MixRenderer &) = delete;

	void set_fade(bool fade) { fade_.store(fade, std::memory_order_relaxed); }
	void render(uint32_t cx, uint32_t cy);

	/* Draws one scene item exactly as its scene would (crop, scale filter, blending, transform), relative to the
	 * current matrix. Used by source mixes to record a source as it is placed in a scene. */
	void render_item(obs_sceneitem_t *item);

private:
	struct Item {
		obs_sceneitem_t *item; /* reference held until end of frame */
		obs_source_t *source;
		uint32_t mask;
		bool visible;
		int child; /* node index if the item is a scene or group, else -1 */
	};

	struct Node {
		obs_source_t *source;
		uint32_t first;
		uint32_t count;
		bool has_hidden;
	};

	struct Channel {
		obs_source_t *source; /* reference held until end of frame */
		obs_source_t *a = nullptr;
		obs_source_t *b = nullptr;
		bool transition = false;
	};

	struct Target {
		gs_texrender_t *tex;
		uint64_t frame;
	};

	int visit(obs_source_t *source, int depth);
	bool hidden_below(obs_source_t *source) const;
	void release_frame();

	void draw_source(obs_source_t *source);
	void draw_node(const Node &node);
	void draw_item(const Item &item);
	void draw_content(const Item &item);
	void draw_transition(const Channel &ch, uint32_t cx, uint32_t cy);
	void draw_fade(obs_source_t *a, obs_source_t *b, float t, uint32_t cx, uint32_t cy);
	void draw_item_texture(gs_texture_t *tex, obs_sceneitem_t *item, const vec2 &output_scale,
			       enum gs_color_space current_space, enum gs_color_space source_space);

	gs_texrender_t *item_target(const obs_sceneitem_t *item, enum gs_color_format format);
	void sweep_targets();

	const uint32_t bit_;
	std::atomic<bool> fade_{true};
	uint64_t frame_ = 0;

	std::vector<Channel> channels_;
	std::vector<Node> nodes_;
	std::vector<Item> items_;
	std::vector<std::vector<Item>> scratch_; /* per recursion depth, keeps capacity across frames */
	std::unordered_map<obs_source_t *, int> node_index_;
	std::unordered_map<const obs_sceneitem_t *, Target> targets_;

	gs_texrender_t *fade_tex_[2] = {};
	gs_effect_t *fade_effect_ = nullptr;
	gs_samplerstate_t *point_sampler_ = nullptr;
	bool fade_effect_failed_ = false;
};

} // namespace vmix
