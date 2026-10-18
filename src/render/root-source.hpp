#pragma once

namespace vmix {

/* Private source type placed on channel 0 of every mix canvas; renders program minus hidden items. */
inline constexpr const char *kRootSourceId = "video_mixes_root";

void register_root_source();

} // namespace vmix
