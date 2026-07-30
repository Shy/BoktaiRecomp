// game_ui.h — Boktai's own section of the in-game settings menu.
//
// The engine builds the common surface (display, audio, save states) and knows
// nothing about photodiodes; the solar sensor's player-facing settings are
// game policy, contributed through RunOptions::ui_extra_items. Same split as
// RunOptions::solar_provider: engine owns the mechanism, game owns the policy.
//
// Everything here is reachable without knowing a single keystroke, which is the
// point — the solar hotkeys remain a shortcut, not the only way in, and they
// are unbound by default.

#ifndef BOKTAI_GAME_UI_H
#define BOKTAI_GAME_UI_H

#include "runtime.h"

namespace boktai {

// Points opts at this game's extra menu items and their handlers. Safe to call
// in builds without the runtime-UI headers: the items are then simply never
// read. Must be called before run_game(); the item array is static storage and
// outlives it.
void game_ui_install(gbarecomp::RunOptions& opts);

}  // namespace boktai

#endif  // BOKTAI_GAME_UI_H
