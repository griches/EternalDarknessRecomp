/*
 * Camera options. The game sets its view once a frame in one function (PAL
 * 0x801E4BDC, run twice a frame): a struct with the eye at +0, the point
 * looked at at +12, the up vector at +24 and the field of view at +48 goes to
 * C_MTXLookAt, and every draw of the world uses that matrix (and its inverse,
 * made just after). Here the LookAt call from that function gets another eye
 * and target: the game's own camera struct is left alone, so its camera logic
 * carries on as before and the view goes back to it the moment the option is
 * off, or a cutscene plays.
 *
 * Free look turns the view about the game camera's eye. Third person puts the
 * eye behind and above the character you play (the game's player object,
 * position at +0) and trails after them as they walk. The right stick turns
 * both (in third person its up and down bring the camera closer or take it
 * further); while aiming they hold still. The game moves the player relative to
 * its own camera, so the controls turn the left stick by the angle between
 * the two views (camera_stick_turn), and up walks away from this camera.
 */
#include "host/camera.h"

#include "host/settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/cpu.h"

#define PI 3.14159265f

typedef struct {
    float x, y, z;
} Vec3;

static Vec3 v3(float x, float y, float z) {
    Vec3 v = {x, y, z};
    return v;
}
static Vec3 add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3 sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(Vec3 a) { return sqrtf(dot(a, a)); }
static Vec3 cross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 0.0f ? scale(a, 1.0f / l) : a;
}
/* v turned by angle (radians) about the unit axis k (Rodrigues). */
static Vec3 rotate(Vec3 v, Vec3 k, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    return add(add(scale(v, c), scale(cross(k, v), s)), scale(k, dot(k, v) * (1.0f - c)));
}
/* a with its part along the unit axis k taken out. */
static Vec3 flatten(Vec3 a, Vec3 k) { return sub(a, scale(k, dot(a, k))); }
static float clampf(float v, float lo, float hi) { return fmaxf(lo, fminf(hi, v)); }

static Vec3 read_vec(CPUState* cpu, u32 address) {
    float f[3];
    for (int i = 0; i < 3; i++) {
        const u32 bits = mem_read32(cpu, address + (u32)i * 4u);
        memcpy(&f[i], &bits, sizeof bits);
    }
    return v3(f[0], f[1], f[2]);
}

static float read_float(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void write_vec(CPUState* cpu, u32 address, Vec3 v) {
    const float f[3] = {v.x, v.y, v.z};
    for (int i = 0; i < 3; i++) {
        u32 bits;
        memcpy(&bits, &f[i], sizeof bits);
        mem_write32(cpu, address + (u32)i * 4u, bits);
    }
}

static const char* const kModeNames[CAMERA_MODE_COUNT] = {"Original", "Free look", "Third person"};

const char* camera_mode_name(int mode) {
    return mode >= 0 && mode < CAMERA_MODE_COUNT ? kModeNames[mode] : "";
}

static s16 g_player_offset;  // the player object's pointer, from r13 (0: not found)

int camera_mode(void) {
    static int env = -2;
    if (env == -2) {
        const char* text = getenv("ED_CAM");
        env = text != NULL && text[0] != '\0' ? atoi(text) : -1;
    }
    int mode = env >= 0 ? env : g_settings.camera;
    if (mode == CAMERA_THIRD_PERSON && g_player_offset == 0)
        mode = CAMERA_FREE_LOOK;
    return mode >= 0 && mode < CAMERA_MODE_COUNT ? mode : CAMERA_ORIGINAL;
}

void camera_set_player_offset(int offset) { g_player_offset = (s16)offset; }

static bool g_view_active;  // camera_view
static Vec3 g_view_eye, g_view_target;

bool camera_view(float eye[3], float target[3]) {
    if (!g_view_active)
        return false;
    eye[0] = g_view_eye.x, eye[1] = g_view_eye.y, eye[2] = g_view_eye.z;
    target[0] = g_view_target.x, target[1] = g_view_target.y, target[2] = g_view_target.z;
    return true;
}

static u32 g_target_controller;  // camera_set_controllers

void camera_set_controllers(unsigned eye, unsigned target) {
    (void)eye;
    g_target_controller = target;
}

/* Where the game's camera is heading to look: the target controller eases
 * from where it looks now toward the point its +112 points at (the thing the
 * player looks at, from the moment they press B). */
static bool game_goal(CPUState* cpu, Vec3* out) {
    if (g_target_controller == 0u)
        return false;
    const u32 goal = mem_read32(cpu, g_target_controller + 112u);
    if (goal < 0x80000000u || goal - 0x80000000u > cpu->ram_size - 16u)
        return false;
    const Vec3 v = read_vec(cpu, goal);
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z))
        return false;
    *out = v;
    return true;
}

static CameraCollide g_collide;
void camera_set_collide(CameraCollide fn) { g_collide = fn; }

// From the controls (the game thread's pad read).
static float g_stick_x, g_stick_y;
static bool g_aiming, g_play;
static float g_turn;  // camera_stick_turn

void camera_set_stick(float x, float y, bool aiming, bool play) {
    static int log = -1;
    if (log < 0)
        log = getenv("ED_CAM_LOG") != NULL;
    if (log && (aiming != g_aiming || play != g_play))
        fprintf(stderr, "[camera] %s%s\n", aiming ? "aiming" : "not aiming", play ? "" : " (cutscene)");
    g_stick_x = x;
    g_stick_y = y;
    g_aiming = aiming;
    g_play = play;
}

bool camera_takes_stick(void) { return camera_mode() != CAMERA_ORIGINAL; }

float camera_stick_turn(void) {
    return camera_mode() != CAMERA_ORIGINAL && !g_aiming ? g_turn : 0.0f;
}

// Turn rates at full stick (radians a second).
#define YAW_RATE (0.8f * PI)
#define PITCH_RATE (0.4f * PI)
// Third person: the point looked at above the player's feet, how far back
// the eye sits from it, and how high (radians above level) it starts.
#define TP_HEIGHT 150.0f
#define TP_DISTANCE 420.0f
// The right stick's up and down bring the eye closer or take it further
// (units a second at full stick), between these.
#define TP_ZOOM_RATE 400.0f
#define TP_CLOSEST 180.0f
#define TP_FARTHEST 800.0f
#define TP_PITCH 0.30f
// Its field of view (degrees, vertical): its own, not the shot's, so the
// view keeps its size through the game's cuts and its zooms.
#define TP_FOV 50.0f
// A sweep's length (seconds) and the share of it spent easing in and out.
#define SWEEP_TIME 0.8f
#define SWEEP_EASE 0.15f
// Walls: the line test's radius, how far short of a wall the eye stops, and
// the nearest it comes to the point it looks at.
#define TP_RADIUS 12.0f
#define TP_MARGIN 30.0f
#define TP_NEAREST 90.0f
// Room enough behind the player not to rise over their head.
#define TP_ROOMY (0.6f * g_distance)

static int g_last_mode = -1;
// Free look: the turn from the game camera's view (yaw about up, pitch).
static float g_yaw, g_pitch;
static Vec3 g_last_eye, g_last_target;
static bool g_have_last;
// Third person: the level direction from the player to the eye, the eye's
// height (radians above level), where the player was, and how far back the
// eye is now (less than g_distance where a wall is in the way).
static Vec3 g_back;
static float g_rise;
static Vec3 g_last_player;
static bool g_have_player;
static bool g_examining;  // camera_set_examining
// The game's field of view: the widest in this shot (since its last cut), and
// whether it has zoomed well in from that (on something the player looks at).
static float g_fov_base, g_fov_last;
static bool g_zoomed, g_zooming_out;
// Where the camera looks: a sweep in a straight line from one point to the
// next (Alex, the thing she looks at, and back), and the turn round behind
// her with it.
static Vec3 g_aim, g_aim_from, g_aim_to;
static Vec3 g_back_from, g_back_to;
static float g_sweep = 1.0f;  // 0..1 along the current sweep
static bool g_aim_set, g_looking_at, g_back_sweeps;
static float g_reach, g_lift;  // g_lift: risen over the player's head, for room
static float g_distance = TP_DISTANCE;  // how far back the player wants it (kept across rooms)

void camera_set_examining(bool examining) { g_examining = examining; }

float camera_field_of_view(float game_fov) {
    return g_view_active && g_last_mode == CAMERA_THIRD_PERSON ? TP_FOV : game_fov;
}

bool camera_shows_player(void) { return g_view_active && g_last_mode == CAMERA_THIRD_PERSON; }

/* The game camera cut to another shot (or a new room): its eye jumped, or it
 * swung round, between one frame and the next. */
static bool game_cut(Vec3 eye, Vec3 target) {
    if (!g_have_last)
        return true;
    const Vec3 d0 = sub(g_last_target, g_last_eye), d1 = sub(target, eye);
    const float l0 = length(d0), l1 = length(d1);
    if (l0 <= 0.0f || l1 <= 0.0f)
        return true;
    return length(sub(eye, g_last_eye)) > 0.25f * l1 + 20.0f || dot(d0, d1) / (l0 * l1) < 0.9f;
}

/* The player's feet, or false when there is no player object. */
static bool player_position(CPUState* cpu, Vec3* out) {
    if (g_player_offset == 0)
        return false;
    const u32 object = mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset);
    if (object < 0x80000000u || object - 0x80000000u > cpu->ram_size - 64u)
        return false;
    *out = read_vec(cpu, object);
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z) && fabsf(out->x) < 1e6f &&
           fabsf(out->y) < 1e6f && fabsf(out->z) < 1e6f;
}

/* Signed angle (radians, counter-clockwise about k) from a to b, level. */
static float level_angle(Vec3 a, Vec3 b, Vec3 k) {
    a = normalize(flatten(a, k));
    b = normalize(flatten(b, k));
    return atan2f(dot(cross(a, b), k), dot(a, b));
}

/* ED_CAM_SNAP=dir,frame,frame...: main RAM to dir/ram-<frame>.bin, frames
 * counted from the first one drawn (finding things in RAM for tests). */
static void snapshot(CPUState* cpu, unsigned long long frame) {
    static const char* snap = (const char*)-1;
    static unsigned long long first_frame;
    if (snap == (const char*)-1) {
        snap = getenv("ED_CAM_SNAP");
        first_frame = frame;
    }
    if (snap == NULL)
        return;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", snap);
    char* list = strchr(dir, ',');
    for (char* p = list; p != NULL; p = strchr(p + 1, ','))
        if (strtoull(p + 1, NULL, 10) == frame - first_frame) {
            *list = '\0';
            char path[600];
            snprintf(path, sizeof path, "%s/ram-%llu.bin", dir, frame - first_frame);
            FILE* f = fopen(path, "wb");
            if (f != NULL) {
                fwrite(cpu->ram, 1, cpu->ram_size, f);
                fclose(f);
            }
            fprintf(stderr, "[camera] RAM to %s\n", path);
            return;
        }
}

/* Free look: the game camera's view turned by g_yaw about up and tipped by
 * g_pitch, from its own eye. A cut to another shot starts it straight again. */
static void free_look(Vec3 eye, Vec3 target, Vec3 k, bool step, bool still, Vec3* new_eye, Vec3* new_target) {
    if (step) {
        if (game_cut(eye, target))
            g_yaw = g_pitch = 0.0f;
        if (!still) {
            const float dt = 1.0f / 60.0f;
            g_yaw = remainderf(g_yaw - g_stick_x * YAW_RATE * dt, 2.0f * PI);
            g_pitch = clampf(g_pitch + g_stick_y * PITCH_RATE * dt, -1.2f, 1.2f);
        }
    }
    const Vec3 dir = sub(target, eye);
    const float dist = length(dir);
    Vec3 look = rotate(scale(dir, 1.0f / dist), k, g_yaw);
    // Tipped about its own right, short of straight up or down.
    const float game_pitch = asinf(clampf(dot(look, k), -1.0f, 1.0f));
    const float pitch = clampf(game_pitch + g_pitch, -1.4f, 1.4f) - game_pitch;
    const Vec3 right = normalize(cross(look, k));
    if (length(right) > 0.0f)
        look = rotate(look, right, pitch);
    *new_eye = eye;
    *new_target = add(eye, scale(look, dist));
}

/* How far out from the pivot along the unit direction out the eye can go:
 * the game's own line test (a little wider than the camera), stopping short
 * of the first wall, floor or ceiling in the way. */
static float wall_reach(CPUState* cpu, Vec3 pivot, Vec3 out) {
    if (g_collide == NULL)
        return g_distance;
    const Vec3 far = add(pivot, scale(out, g_distance + TP_MARGIN));
    const float from[3] = {pivot.x, pivot.y, pivot.z}, to[3] = {far.x, far.y, far.z};
    float hit[3];
    if (!g_collide(cpu, from, to, TP_RADIUS, hit))
        return g_distance;
    return clampf(length(sub(v3(hit[0], hit[1], hit[2]), pivot)) - TP_MARGIN, TP_NEAREST, g_distance);
}

/* A sweep's progress (0..1 of its time) to how far along the line it is:
 * even speed, easing in and out over SWEEP_EASE of the time at each end. */
static float sweep_ease(float t) {
    const float a = SWEEP_EASE, v = 1.0f / (1.0f - a);
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    if (t < a)
        return v * t * t / (2.0f * a);
    if (t > 1.0f - a)
        return 1.0f - v * (1.0f - t) * (1.0f - t) / (2.0f * a);
    return v * (t - a * 0.5f);
}

/* Once a frame: what the camera looks at. In play that is the player; when
 * they look closely at something (the game hides them, or zooms in), the
 * point the game's camera is heading for. A change starts a sweep from where
 * the camera looks now, and with a thing to look at it also turns round
 * behind the player on the line to it (unless the stick is turning it). */
static void sweep_aim(Vec3 focus, Vec3 pivot, Vec3 k, bool jumped) {
    const bool looking = g_examining || (g_zoomed && !g_zooming_out);
    const Vec3 goal = looking ? focus : pivot;
    if (jumped || !g_aim_set) {
        g_aim = g_aim_from = g_aim_to = pivot;
        g_sweep = 1.0f;
        g_aim_set = true;
        g_looking_at = false;
        g_back_sweeps = false;
    }
    const bool moved = looking && length(sub(goal, g_aim_to)) > 20.0f;
    if (looking != g_looking_at || moved) {
        g_aim_from = g_aim;
        g_sweep = 0.0f;
        g_looking_at = looking;
        g_back_sweeps = false;
        const Vec3 behind = normalize(flatten(sub(pivot, goal), k));
        if (looking && length(behind) > 0.0f) {
            g_back_from = g_back;
            g_back_to = behind;
            g_back_sweeps = true;
        }
    }
    g_aim_to = goal;  // back to the player: wherever they are now
    g_sweep = fminf(1.0f, g_sweep + 1.0f / (60.0f * SWEEP_TIME));
    const float e = sweep_ease(g_sweep);
    g_aim = add(g_aim_from, scale(sub(g_aim_to, g_aim_from), e));
    if (fabsf(g_stick_x) >= 0.1f)
        g_back_sweeps = false;
    static int log = -1;
    if (log < 0)
        log = getenv("ED_CAM_LOG") != NULL && getenv("ED_CAM_LOG")[0] == '3';
    if (log && g_sweep < 1.0f)
        fprintf(stderr, "[camera] sweep %.3f aim %.1f %.1f %.1f\n", g_sweep, g_aim.x, g_aim.y, g_aim.z);
    if (g_back_sweeps) {
        const float angle = level_angle(g_back_from, g_back_to, k);
        g_back = normalize(rotate(g_back_from, k, angle * e));
    }
}

/* Third person: behind and above the player, trailing after them. Starts
 * from the side the game's camera is on. While the player looks closely at
 * something, the game's camera looks at it (focus): this one turns to it too,
 * from behind the player, so a thing high on a wall comes into view. */
static void third_person(CPUState* cpu, Vec3 eye, Vec3 focus, Vec3 player, Vec3 k, bool step, bool still,
                         Vec3* new_eye, Vec3* new_target) {
    const Vec3 pivot = add(player, scale(k, TP_HEIGHT));
    if (step) {
        const bool jumped = !g_have_player || length(sub(player, g_last_player)) > 300.0f;
        sweep_aim(focus, pivot, k, jumped);
        if (jumped) {
            g_back = normalize(flatten(sub(eye, pivot), k));
            if (length(g_back) == 0.0f)
                g_back = normalize(flatten(v3(1.0f, 0.0f, 0.0f), k));
            g_rise = TP_PITCH;
        } else if (!still) {
            // Trail: the eye stays where it was as the player walks off,
            // drawn back out to its distance along the line to them.
            const Vec3 old_eye = add(add(g_last_player, scale(k, TP_HEIGHT)),
                                     scale(g_back, g_distance * cosf(g_rise)));
            const Vec3 back = normalize(flatten(sub(old_eye, pivot), k));
            if (length(back) > 0.0f)
                g_back = back;
            const float dt = 1.0f / 60.0f;
            g_back = normalize(rotate(g_back, k, -g_stick_x * YAW_RATE * dt));
            g_distance = clampf(g_distance - g_stick_y * TP_ZOOM_RATE * dt, TP_CLOSEST, TP_FARTHEST);
        }
        // Walls: where the eye would be has room enough, or it rises over
        // the player's head (looking down on them) to where there is more,
        // easing up and back down.
        float best = -1.0f, lift = 0.0f;
        for (int i = 0; i < 4 && g_collide != NULL; i++) {
            const float rise = fminf(g_rise + 0.3f * (float)i, 1.35f);
            const float reach = wall_reach(cpu, pivot, add(scale(g_back, cosf(rise)), scale(k, sinf(rise))));
            if (reach > best + 1.0f) {
                best = reach;
                lift = rise - g_rise;
            }
            if (reach >= TP_ROOMY)
                break;
        }
        g_lift = jumped ? lift : g_lift + clampf(lift - g_lift, -1.5f / 60.0f, 1.5f / 60.0f);
        // Then the eye comes in short of the first thing in the way at once,
        // and goes back out gently once it clears.
        const float rise = g_rise + g_lift;
        const float reach = wall_reach(cpu, pivot, add(scale(g_back, cosf(rise)), scale(k, sinf(rise))));
        g_reach = jumped || reach < g_reach ? reach : fminf(reach, g_reach + TP_FARTHEST * 1.5f / 60.0f);
        g_last_player = player;
        g_have_player = true;
    }
    const float rise = g_rise + g_lift;
    const Vec3 out = add(scale(g_back, cosf(rise)), scale(k, sinf(rise)));
    *new_eye = add(pivot, scale(out, g_reach));
    *new_target = g_aim;
}

static bool g_log_checked, g_log;

/* Called at C_MTXLookAt from the game's camera (hle.c checks the return
 * address): r4 = eye, r5 = up, r6 = target; r1 = the camera function's stack
 * frame, whose bytes 40..63 it never uses (the eye and target written there
 * are read once, by LookAt). The world's up is the camera's up vector (+Z in
 * this game). */
void camera_look_at(CPUState* cpu) {
    if (!g_log_checked) {
        g_log_checked = true;
        g_log = getenv("ED_CAM_LOG") != NULL;
    }
    const Vec3 eye = read_vec(cpu, cpu->gpr[4]), up = read_vec(cpu, cpu->gpr[5]),
               target = read_vec(cpu, cpu->gpr[6]);
    const unsigned long long frame = cpu->timebase / 675000ull;
    static unsigned long long last_frame;
    const bool step = frame != last_frame;  // once a frame (the camera runs twice)
    last_frame = frame;
    if (step)
        snapshot(cpu, frame);
    Vec3 player = v3(0.0f, 0.0f, 0.0f);
    const bool have_player = player_position(cpu, &player);
    if (g_log && step && (frame % 120u == 0u || (getenv("ED_CAM_LOG")[0] == '2' && frame % 10u == 0u)))
        fprintf(stderr, "[camera] frame %llu eye %.1f %.1f %.1f target %.1f %.1f %.1f up %.2f %.2f %.2f player %s%.1f %.1f %.1f fov %.1f%s\n",
                frame, eye.x, eye.y, eye.z, target.x, target.y, target.z, up.x, up.y, up.z,
                have_player ? "" : "(none) ", player.x, player.y, player.z, read_float(cpu, cpu->gpr[4] + 48u),
                g_examining ? " examining" : g_zoomed ? " zoomed" : "");
    if (g_log && step && getenv("ED_CAM_LOG")[0] == '2' && frame % 10u == 0u) {
        Vec3 goal;
        if (game_goal(cpu, &goal))
            fprintf(stderr, "[camera]   goal %.1f %.1f %.1f\n", goal.x, goal.y, goal.z);
    }
    // The game zooms its camera in on what the player looks at (B) without
    // hiding them: a field of view well under the shot's own. A cut to another
    // shot starts the comparison again.
    if (step) {
        const float fov = read_float(cpu, cpu->gpr[4] + 48u);
        if (game_cut(eye, target) || g_fov_base <= 0.0f || fov > g_fov_base)
            g_fov_base = fov;
        g_zoomed = fov < 0.8f * g_fov_base;
        // Zooming back out: the look is over (its goal goes back to the
        // shot's own point first).
        g_zooming_out = fov > g_fov_last + 0.01f;
        g_fov_last = fov;
    }
    // Only in play: a character to play, and no cutscene.
    int mode = g_play && have_player ? camera_mode() : CAMERA_ORIGINAL;
    const Vec3 k = normalize(up);
    if (length(sub(target, eye)) <= 0.0f || length(k) <= 0.0f)
        mode = CAMERA_ORIGINAL;
    if (mode != g_last_mode && step) {
        // A fresh start in the new mode.
        g_have_last = false;
        g_have_player = false;
        if (g_log)
            fprintf(stderr, "[camera] %s from frame %llu\n", camera_mode_name(mode), frame);
        g_last_mode = mode;
    }
    if (mode == CAMERA_ORIGINAL) {
        g_turn = 0.0f;
        g_view_active = false;
        return;
    }
    const bool still = g_aiming;
    Vec3 new_eye, new_target;
    if (mode == CAMERA_FREE_LOOK)
        free_look(eye, target, k, step, still, &new_eye, &new_target);
    else {
        // What the player looks at: where the game's camera is heading,
        // known from the moment they press B (else where it looks now).
        Vec3 focus = target;
        game_goal(cpu, &focus);
        third_person(cpu, eye, focus, player, k, step, still, &new_eye, &new_target);
    }
    if (step) {
        g_last_eye = eye;
        g_last_target = target;
        g_have_last = true;
        if (!still)
            g_turn = level_angle(sub(target, eye), sub(new_target, new_eye), k);
    }
    g_view_active = true;
    g_view_eye = new_eye;
    g_view_target = new_target;
    const u32 sp = cpu->gpr[1];
    write_vec(cpu, sp + 40u, new_eye);
    write_vec(cpu, sp + 52u, new_target);
    cpu->gpr[4] = sp + 40u;
    cpu->gpr[6] = sp + 52u;
}
