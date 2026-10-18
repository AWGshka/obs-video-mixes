#pragma once

#include <obs.h>

#include <cstdint>

namespace vmix {

/*
 * Per scene item "hidden in mix" bitmask, stored in the item's private settings so it is saved with the
 * scene collection and follows duplicated/copied items.
 *
 * Reads happen on the graphics thread, writes on the UI thread; obs_data is not safe for concurrent
 * read/write, so both go through one mutex. It is always the innermost lock (never held while taking
 * a scene lock), so it cannot participate in a lock-order inversion.
 */
uint32_t hidden_mask(obs_sceneitem_t *item);
void set_hidden(obs_sceneitem_t *item, uint32_t mix_bit, bool hidden);

/* Removes a mix bit from every scene item of the loaded scene collection (used when a mix is deleted). */
void clear_bit_everywhere(uint32_t mix_bit);

} // namespace vmix
