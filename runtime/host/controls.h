#pragma once
// Controller and keyboard layouts for the game's pad (port 0): Original
// (the GameCube positions), Modern, or Custom (rebound in the Controls menu).
#include <stdbool.h>

#ifdef __cplusplus
#include <cstdint>
#include <string>
#include <vector>

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

// For the game's own button icons (buttons.cpp): the icon file under
// res/ui/glyphs for an action, preferring a face button or a shoulder
// button when the action has one (empty when unbound); with no controller,
// the action's key. Shoulder cells take a trigger over a bumper in play and
// a bumper in the game's pause menu, where L and R page it
// (controls_set_game_menu). glyph_set() is "xbox", "ps", "switch" or "kb".
enum GlyphPreference { kAnyButton, kFaceButton, kShoulderButton };
std::string action_glyph(int action, int preference);
std::string glyph_set();
// For the Tome's controls page (tome.cpp): the controller inputs bound to an
// action in play, as icon names ("rt", "south"), and the bound key's icon
// ("kb/j", or "" when it has none). bindings_signature() changes when either
// would (layout, bindings, controller family).
std::vector<std::string> pad_input_ids(int action);
std::string key_glyph_file(int action);
uint64_t bindings_signature();
// Changes whenever the icons to show would change (layout, bindings,
// controller family plugged in or out, the pause menu opening or closing).
uint64_t glyph_signature();

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
// Whether the game's pause menu is open (hle.c, each pad read).
void controls_set_game_menu(bool open);
// The guest frame (timebase / 675000) of this pad read, for ED_PAD_SCRIPT.
void controls_note_frame(unsigned long long frame);
#ifdef __cplusplus
}
#endif
