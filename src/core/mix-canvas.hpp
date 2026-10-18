#pragma once

#include "core/config.hpp"

#include <obs.hpp>

#include <memory>
#include <string>

namespace vmix {

/*
 * A private canvas whose only channel is the mix root source. libobs renders every canvas every frame
 * whether or not anything consumes it, so a MixCanvas only exists while a recording or preview needs it.
 *
 * Program mixes use the program canvas size. Source mixes use the size the source occupies on screen, i.e. its
 * scene item with crop/scale/bounds/rotation applied (its own size if it is not placed in any scene), or an
 * explicit size with the source letterboxed into it. They keep the source showing/active, so e.g. a webcam that
 * is not in the current scene still delivers frames.
 */
class MixCanvas {
public:
	static std::unique_ptr<MixCanvas> create(const MixConfig &cfg, std::string &error);
	~MixCanvas();

	MixCanvas(const MixCanvas &) = delete;
	MixCanvas &operator=(const MixCanvas &) = delete;

	obs_canvas_t *canvas() const { return canvas_; }
	video_t *video() const { return obs_canvas_get_video(canvas_); }

	void set_fade(bool fade);

private:
	MixCanvas() = default;

	OBSSource target_; /* source mixes only; holds a showing + active reference */
	OBSCanvas canvas_;
	OBSSource root_;
};

} // namespace vmix
