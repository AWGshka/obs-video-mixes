#pragma once

#include <obs.h>

#include <functional>
#include <memory>

namespace vmix {

/*
 * Scene and group filters for subtrees a mix re-renders itself.
 *
 * A filter chain renders its parent through libobs (for a scene: the full scene, hidden items included) and caches
 * the result once per frame, so the original filters cannot be reused for a mix. Instead each mix gets a private
 * proxy source per filtered scene/group, carrying private copies of that scene's video filters. Rendering the proxy
 * runs the copied filter chain over whatever the mix draws as the proxy's content.
 *
 * Copies are made on the UI thread (creating sources on the graphics thread could deadlock against the UI thread)
 * and kept in sync: settings through each original's "update" signal, enabled state every frame, and added/removed/
 * reordered filters by rebuilding the copies. Until a copy set exists, the subtree is drawn without filters.
 */
class SceneFilters {
public:
	SceneFilters();
	~SceneFilters(); /* graphics thread */

	SceneFilters(const SceneFilters &) = delete;
	SceneFilters &operator=(const SceneFilters &) = delete;

	/*
	 * Graphics thread. If `scene` has video filters, draws `content` (in scene coordinates) through copies of them
	 * and returns true. Returns false if it has none, or the copies are not ready yet: draw `content` directly.
	 */
	bool render(obs_source_t *scene, const std::function<void()> &content);

	/* Graphics thread, once per frame: drops copies of scenes not drawn in the last frames. */
	void end_frame();

	struct State;

private:
	std::shared_ptr<State> state_;
	uint64_t frame_ = 0;
};

/* Registers the proxy source type; call once at module load. */
void register_scene_filter_proxy();

} // namespace vmix
