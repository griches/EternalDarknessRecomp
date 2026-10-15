// The game's pad (port 0) built from the controller and the keyboard through
// a layout, so any action can sit on any button, trigger or key. Aurora still
// reads the controller for rumble; dol_aurora_set_pad0_override replaces only
// the state the game sees.
#include "host/controls.h"

#include "gxruntime/aurora_backend.h"
#include "host/settings.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace controls {
namespace {

// Controller inputs (SDL's positional names) plus the analog triggers.
enum Native {
    kNone = -1,
    kSouth, kEast, kWest, kNorth,
    kLB, kRB, kLT, kRT, kL3, kR3,
    kStart,
    kDUp, kDDown, kDLeft, kDRight,
    kNativeCount,
};

const char* const kNativeIds[kNativeCount] = {
    "south", "east", "west", "north", "lb", "rb", "lt", "rt",
    "l3", "r3", "start", "dup", "ddown", "dleft", "dright",
};

struct Binding {
    int pad[2];  // up to two controller inputs (kNone when unused)
    int key;     // SDL scancode (0 when none)
};

// GameCube output of each action.
const unsigned short kGcButton[kActionCount] = {
    0x0100 /* A */, 0x0200 /* B */, 0x0400 /* X */, 0x0800 /* Y */,
    0x0010 /* Z */, 0x0040 /* L */, 0x0020 /* R */, 0x1000 /* Start */,
    0x0008 /* up */, 0x0004 /* down */, 0x0001 /* left */, 0x0002 /* right */,
};

const char* const kActionNames[kActionCount] = {
    "Attack / Confirm", "Action / Cancel", "Creep", "Quickcast", "Reload",
    "Sprint", "Target", "Pause", "Spell up", "Spell down", "Spell left",
    "Spell right",
};

const char* const kActionIds[kActionCount] = {
    "attack", "action", "creep", "quickcast", "reload", "sprint", "target",
    "pause", "spell_up", "spell_down", "spell_left", "spell_right",
};

// The keyboard is the same in every layout until it is rebound.
constexpr int kKeys[kActionCount] = {
    SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_U, SDL_SCANCODE_I,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
};

// GameCube positions, as Aurora maps a controller by default.
const int kOriginalPad[kActionCount][2] = {
    {kSouth, kNone}, {kEast, kNone}, {kWest, kNone}, {kNorth, kNone},
    {kRB, kNone},    {kLT, kNone},   {kRT, kNone},   {kStart, kNone},
    {kDUp, kNone},   {kDDown, kNone}, {kDLeft, kNone}, {kDRight, kNone},
};

// Modern: attack and confirm on RT and A, action and cancel on B, aim on LT,
// sprint and creep on the stick clicks (toggles by default), reload X,
// quickcast Y. The GameCube L and R also sit on LB and RB, which page the
// game's own menus both ways (in play LB sprints, RB targets, while held).
const int kModernPad[kActionCount][2] = {
    {kRT, kSouth}, {kEast, kNone}, {kR3, kNone},    {kNorth, kNone},
    {kWest, kNone}, {kL3, kLB},    {kLT, kRB},      {kStart, kNone},
    {kDUp, kNone},  {kDDown, kNone}, {kDLeft, kNone}, {kDRight, kNone},
};

Binding g_bindings[kActionCount];
int g_layout = 0;
SDL_Gamepad* g_pad = nullptr;

int g_capture = -1;         // action waiting for an input, or -1
Uint64 g_capture_deadline = 0;
bool g_capture_armed = false;  // every input released since capture began

SDL_Gamepad* gamepad() {
    if (g_pad != nullptr && SDL_GamepadConnected(g_pad))
        return g_pad;
    g_pad = nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr && count > 0)
        g_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    return g_pad;
}

// 0-255 for a native input (buttons are 0 or 255).
int native_value(SDL_Gamepad* pad, int n) {
    if (pad == nullptr || n == kNone)
        return 0;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b) ? 255 : 0; };
    auto trigger = [&](SDL_GamepadAxis a) {
        return std::clamp(SDL_GetGamepadAxis(pad, a) * 255 / 32767, 0, 255);
    };
    switch (n) {
    case kSouth: return button(SDL_GAMEPAD_BUTTON_SOUTH);
    case kEast: return button(SDL_GAMEPAD_BUTTON_EAST);
    case kWest: return button(SDL_GAMEPAD_BUTTON_WEST);
    case kNorth: return button(SDL_GAMEPAD_BUTTON_NORTH);
    case kLB: return button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    case kRB: return button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    case kLT: return trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    case kRT: return trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    case kL3: return button(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    case kR3: return button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    case kStart: return button(SDL_GAMEPAD_BUTTON_START);
    case kDUp: return button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    case kDDown: return button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    case kDLeft: return button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    case kDRight: return button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    default: return 0;
    }
}

bool key_down(int scancode) {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys != nullptr && scancode > 0 && scancode < count && keys[scancode];
}

// SDL stick axis (-32768..32767) to the GameCube's -127..127, with a deadzone.
int stick(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool invert) {
    if (pad == nullptr)
        return 0;
    int v = SDL_GetGamepadAxis(pad, axis);
    if (invert)
        v = -v - 1;
    if (std::abs(v) < g_settings.stick_deadzone * 32767 / 100)
        return 0;
    return std::clamp(v / 256, -127, 127);
}

enum Brand { kXbox, kPlayStation, kNintendo };

Brand brand() {
    SDL_Gamepad* pad = gamepad();
    if (pad == nullptr)
        return kXbox;
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return kNintendo;
    default:
        return kXbox;
    }
}

const char* native_name(int n) {
    static const char* const xbox[kNativeCount] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Menu",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
    static const char* const ps[kNativeCount] = {
        "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
        "Options", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
    static const char* const nintendo[kNativeCount] = {
        "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "L-stick", "R-stick", "+",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
    if (n < 0 || n >= kNativeCount)
        return "-";
    switch (brand()) {
    case kPlayStation: return ps[n];
    case kNintendo: return nintendo[n];
    default: return xbox[n];
    }
}

void load_preset(int layout) {
    const int (*table)[2] = layout == kModern ? kModernPad : kOriginalPad;
    for (int a = 0; a < kActionCount; a++)
        g_bindings[a] = {{table[a][0], table[a][1]}, kKeys[a]};
}

int native_from_id(const std::string& id) {
    for (int n = 0; n < kNativeCount; n++)
        if (id == kNativeIds[n])
            return n;
    return kNone;
}

// settings.ini: controls_custom = attack=rt+south:13;action=east:14;...
std::string serialize() {
    std::string out;
    for (int a = 0; a < kActionCount; a++) {
        if (!out.empty())
            out += ';';
        out += kActionIds[a];
        out += '=';
        bool first = true;
        for (int n : g_bindings[a].pad)
            if (n != kNone) {
                if (!first)
                    out += '+';
                out += kNativeIds[n];
                first = false;
            }
        out += ':' + std::to_string(g_bindings[a].key);
    }
    return out;
}

void parse(const char* text) {
    load_preset(kOriginal);
    std::string s(text);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos)
            end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        const size_t colon = item.find(':');
        if (eq == std::string::npos || colon == std::string::npos || colon < eq)
            continue;
        const std::string id = item.substr(0, eq);
        int action = -1;
        for (int a = 0; a < kActionCount; a++)
            if (id == kActionIds[a])
                action = a;
        if (action < 0)
            continue;
        Binding b{{kNone, kNone}, std::atoi(item.c_str() + colon + 1)};
        const std::string pads = item.substr(eq + 1, colon - eq - 1);
        const size_t plus = pads.find('+');
        b.pad[0] = native_from_id(pads.substr(0, plus));
        if (plus != std::string::npos)
            b.pad[1] = native_from_id(pads.substr(plus + 1));
        g_bindings[action] = b;
    }
}

void save() {
    g_settings.controls_layout = g_layout;
    if (g_layout == kCustom)
        snprintf(g_settings.controls_custom, sizeof g_settings.controls_custom, "%s",
                 serialize().c_str());
    settings_save();
}

// Toggle modes (sprint, creep, target): a press flips the action on or off.
bool g_toggled[kActionCount];
bool g_was_down[kActionCount];
Uint64 g_still_since = 0;  // when the main stick last came to rest

bool toggle_mode(int action) {
    return (action == kSprint && g_settings.sprint_toggle[g_layout]) ||
           (action == kCreep && g_settings.creep_toggle[g_layout]) ||
           (action == kTarget && g_settings.target_toggle[g_layout]);
}

// Custom starts from the layout in use: its buttons and hold/toggle choices.
void custom_from(int layout) {
    g_settings.sprint_toggle[kCustom] = g_settings.sprint_toggle[layout];
    g_settings.creep_toggle[kCustom] = g_settings.creep_toggle[layout];
    g_settings.target_toggle[kCustom] = g_settings.target_toggle[layout];
}

bool read_pad(DolPadState* state) {
    SDL_Gamepad* pad = gamepad();
    DolPadState s{};
    // The main stick first: toggled sprint ends when you stop moving.
    int x = stick(pad, SDL_GAMEPAD_AXIS_LEFTX, false);
    int y = stick(pad, SDL_GAMEPAD_AXIS_LEFTY, true);
    if (key_down(SDL_SCANCODE_A)) x = -127;
    if (key_down(SDL_SCANCODE_D)) x = 127;
    if (key_down(SDL_SCANCODE_W)) y = 127;
    if (key_down(SDL_SCANCODE_S)) y = -127;
    const Uint64 now = SDL_GetTicks();
    if (x != 0 || y != 0)
        g_still_since = 0;
    else if (g_still_since == 0)
        g_still_since = now;
    if (g_toggled[kSprint] && g_still_since != 0 && now - g_still_since > 300)
        g_toggled[kSprint] = false;
    for (int a = 0; a < kActionCount; a++) {
        // In toggle mode only the first button and the key toggle; a second
        // button (Modern's bumpers) is a plain hold, so it pages menus once.
        int v = native_value(pad, g_bindings[a].pad[0]);
        const int held = native_value(pad, g_bindings[a].pad[1]);
        if (key_down(g_bindings[a].key))
            v = 255;
        if (!toggle_mode(a))
            v = std::max(v, held);
        if (toggle_mode(a)) {
            const bool down = v >= 128;
            if (down && !g_was_down[a]) {
                g_toggled[a] = !g_toggled[a];
                // Sprinting and creeping cancel each other.
                if (g_toggled[a] && a == kSprint)
                    g_toggled[kCreep] = false;
                if (g_toggled[a] && a == kCreep)
                    g_toggled[kSprint] = false;
            }
            g_was_down[a] = down;
            v = std::max(g_toggled[a] ? 255 : 0, held);
        } else {
            g_toggled[a] = false;
        }
        if (a == kSprint || a == kTarget) {
            // Analog L/R: the trigger value; the digital click at full press.
            (a == kSprint ? s.trigger_left : s.trigger_right) = static_cast<u8>(v);
            if (v >= 230)
                s.button |= kGcButton[a];
        } else if (v >= 128) {
            s.button |= kGcButton[a];
        }
    }
    s.analog_a = (s.button & 0x0100) ? 255 : 0;
    s.analog_b = (s.button & 0x0200) ? 255 : 0;
    // C-stick: right stick or TFGH.
    int cx = stick(pad, SDL_GAMEPAD_AXIS_RIGHTX, false);
    int cy = stick(pad, SDL_GAMEPAD_AXIS_RIGHTY, true);
    if (key_down(SDL_SCANCODE_F)) cx = -127;
    if (key_down(SDL_SCANCODE_H)) cx = 127;
    if (key_down(SDL_SCANCODE_T)) cy = 127;
    if (key_down(SDL_SCANCODE_G)) cy = -127;
    s.stick_x = static_cast<s8>(x);
    s.stick_y = static_cast<s8>(y);
    s.substick_x = static_cast<s8>(cx);
    s.substick_y = static_cast<s8>(cy);
    *state = s;
    return true;
}

}  // namespace

const char* action_name(int action) {
    return action >= 0 && action < kActionCount ? kActionNames[action] : "";
}

const char* layout_name(int layout) {
    static const char* const names[kLayoutCount] = {"Original", "Modern", "Custom"};
    return layout >= 0 && layout < kLayoutCount ? names[layout] : "";
}

std::string pad_label(int action) {
    std::string out;
    for (int n : g_bindings[action].pad)
        if (n != kNone) {
            if (!out.empty())
                out += " / ";
            out += native_name(n);
        }
    return out.empty() ? "-" : out;
}

std::string key_label(int action) {
    const int key = g_bindings[action].key;
    if (key <= 0)
        return "-";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(key));
    return name != nullptr && name[0] != '\0' ? name : "?";
}

namespace {
std::string glyph_img(const std::string& file, const std::string& alt) {
    // Paths are relative to res/ui/menu.rml.
    return "<img class=\"glyph\" src=\"glyphs/" + file + ".png\" alt=\"" + alt + "\"/>";
}
}  // namespace

std::string pad_glyphs(int action) {
    const char* set = brand() == kPlayStation ? "ps" : brand() == kNintendo ? "switch" : "xbox";
    std::string out;
    for (int n : g_bindings[action].pad)
        if (n != kNone)
            out += glyph_img(std::string(set) + "/" + kNativeIds[n], native_name(n));
    return out.empty() ? "<span class=\"unbound\">-</span>" : out;
}

std::string key_glyph(int action) {
    const int key = g_bindings[action].key;
    if (key <= 0)
        return "<span class=\"unbound\">-</span>";
    // SDL's key name to the icon file: "Return" -> return, "Up" -> arrow_up,
    // "Left Shift" -> shift; keys without an icon show their name in a keycap.
    std::string name = key_label(action);
    std::string file;
    for (char c : name)
        file += c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (file == "up" || file == "down" || file == "left" || file == "right")
        file = "arrow_" + file;
    else if (file == "left_shift" || file == "right_shift")
        file = "shift";
    else if (file == "left_ctrl" || file == "right_ctrl")
        file = "ctrl";
    else if (file == "left_alt" || file == "right_alt")
        file = "alt";
    else if (file == "left_gui" || file == "right_gui")
        file = "command";
    else if (file == "pageup")
        file = "page_up";
    else if (file == "pagedown")
        file = "page_down";
    else if (file == "keypad_enter")
        file = "numpad_enter";
    static const char* const kIcons[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f", "g",
        "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
        "y", "z", "alt", "arrow_down", "arrow_left", "arrow_right", "arrow_up", "backspace",
        "capslock", "command", "ctrl", "delete", "end", "enter", "escape", "f1", "f2", "f3",
        "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "home", "insert",
        "numpad_enter", "page_down", "page_up", "return", "shift", "space", "tab"};
    for (const char* icon : kIcons)
        if (file == icon)
            return glyph_img("kb/" + file, name);
    return "<span class=\"keycap\">" + name + "</span>";
}

std::string controller_name() {
    SDL_Gamepad* pad = gamepad();
    if (pad == nullptr)
        return "No controller (keyboard)";
    const char* name = SDL_GetGamepadName(pad);
    return name != nullptr ? name : "Controller";
}

void set_layout(int layout) {
    if (layout == kCustom) {
        if (g_layout != kCustom && g_settings.controls_custom[0] != '\0')
            parse(g_settings.controls_custom);  // the saved custom layout
        else if (g_layout != kCustom)
            custom_from(g_layout);  // a first Custom starts from the layout in use
    } else {
        load_preset(layout);
    }
    g_layout = layout;
    save();
}

int layout() { return g_layout; }

void begin_capture(int action) {
    if (g_layout != kCustom) {
        custom_from(g_layout);  // start Custom from what is in use
        g_layout = kCustom;
    }
    g_capture = action;
    g_capture_armed = false;
    g_capture_deadline = SDL_GetTicks() + 6000;
}

bool capturing() { return g_capture >= 0; }
int capture_action() { return g_capture; }

bool poll_capture() {
    if (g_capture < 0)
        return true;
    SDL_Gamepad* pad = gamepad();
    bool any = false;
    int pressed = kNone;
    for (int n = 0; n < kNativeCount; n++) {
        const int v = native_value(pad, n);
        if (v >= 200) {
            any = true;
            if (pressed == kNone)
                pressed = n;
        }
    }
    if (!g_capture_armed) {
        // Wait for the button that started the capture to be let go.
        if (!any)
            g_capture_armed = true;
    } else if (pressed != kNone) {
        g_bindings[g_capture].pad[0] = pressed;
        g_bindings[g_capture].pad[1] = kNone;
        g_capture = -1;
        save();
        return true;
    }
    if (SDL_GetTicks() > g_capture_deadline) {
        g_capture = -1;
        save();
        return true;
    }
    return false;
}

void capture_key(int scancode) {
    if (g_capture < 0 || scancode <= 0)
        return;
    if (scancode == SDL_SCANCODE_ESCAPE) {  // cancel
        g_capture = -1;
        return;
    }
    g_bindings[g_capture].key = scancode;
    g_capture = -1;
    save();
}

}  // namespace controls

extern "C" void controls_init(void) {
    using namespace controls;
    g_layout = std::clamp(g_settings.controls_layout, 0, static_cast<int>(kLayoutCount) - 1);
    if (g_layout == kCustom && g_settings.controls_custom[0] != '\0')
        parse(g_settings.controls_custom);
    else
        load_preset(g_layout == kCustom ? kOriginal : g_layout);
    dol_aurora_set_pad0_override(read_pad);
}
