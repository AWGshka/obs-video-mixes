#pragma once

#include "core/config.hpp"

class QString;
class QWidget;

namespace vmix {
struct MixState;
}

namespace vmix::ui {

enum class SettingsResult { Cancelled, Saved, Deleted };

/*
 * Edits cfg in place when Saved. state is the mix's live state (outputs that are running lock their own settings),
 * or nullptr for a mix that does not exist yet (the dialog then offers "Add mix" instead of "Delete").
 */
SettingsResult edit_mix_settings(QWidget *parent, MixConfig &cfg, const MixState *state);

#ifdef VMIX_DEV_SNAPSHOTS
/* Developer aid: renders every page of the dialog to <prefix>-<page>.png. */
void snapshot_settings(QWidget *parent, const MixConfig &cfg, const MixState *state, const QString &prefix);
#endif

} // namespace vmix::ui
