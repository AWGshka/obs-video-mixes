#pragma once

#include "core/config.hpp"

class QWidget;

namespace vmix::ui {

enum class SettingsResult { Cancelled, Saved, Deleted };

/* Edits cfg in place when Saved. Output-related fields are locked while the mix is recording. */
SettingsResult edit_mix_settings(QWidget *parent, MixConfig &cfg, bool recording);

} // namespace vmix::ui
