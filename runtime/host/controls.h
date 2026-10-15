#pragma once
// Controller and keyboard layouts for the game's pad (port 0): Original
// (the GameCube positions), Modern, or Custom (rebound in the Controls menu).
#include <stdbool.h>

#ifdef __cplusplus
#include <string>

namespace controls {

enum Action {
    kAttack,     // GameCube A (also Confirm in the game's menus)
    kAction,     // B: doors, ladders, pick up (also Cancel)
    kCreep,      // X
    kQuickcast,  // Y
    kReload,     // Z
    kSprint,     // L
    kTarget,     // R
    kPause,      // Start
    kSpellUp,    // d-pad: the four quickcast spell slots
    kSpellDown,
    kSpellLeft,
    kSpellRight,
    kActionCount,
};

enum Layout { kOriginal, kModern, kCustom, kLayoutCount };

const char* action_name(int action);
const char* layout_name(int layout);
// The current controller's buttons for an action ("RT / A"), and its key.
std::string pad_label(int action);
std::string key_label(int action);
// The same as RmlUi markup with button and key icons (res/ui/glyphs), for the
// connected controller's family; text where there is no icon.
std::string pad_glyphs(int action);
std::string key_glyph(int action);
// "Xbox controller", "DualSense", ... or "No controller".
std::string controller_name();

// Switch layout (Custom starts from the layout in use).
void set_layout(int layout);
int layout();

// Rebinding (Custom): begin waiting for an input for one action; poll once a
// frame until it returns true (bound, or timed out). A key event from the
// menu's key listener can bind the key instead.
void begin_capture(int action);
bool poll_capture();
void capture_key(int sdl_scancode);
bool capturing();
int capture_action();

}  // namespace controls
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Load the layout from settings and take over pad port 0.
void controls_init(void);
#ifdef __cplusplus
}
#endif
