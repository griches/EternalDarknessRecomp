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
 * Third person puts the eye behind and above the character you play (the
 * game's player object, position at +0) and trails after them as they walk;
 * first person puts it at their eyes. The right stick turns both (in third
 * person its up and down bring the camera closer or take it further); while
 * aiming they hold still. The game moves the player relative to
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

static const char* const kModeNames[CAMERA_MODE_COUNT] = {"Original", "Original", "Third person",
                                                            "First person"};

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
    if (mode != CAMERA_THIRD_PERSON && mode != CAMERA_FIRST_PERSON)
        return CAMERA_ORIGINAL;
    return g_player_offset != 0 ? mode : CAMERA_ORIGINAL;
}

void camera_set_player_offset(int offset) { g_player_offset = (s16)offset; }

static bool g_view_active;  // camera_view
static Vec3 g_view_eye, g_view_target;
static Vec3 g_fp_eye;  // first person's own eye, at the player's head
static Vec3 g_head_eye;  // the player's eyes (any mode)
// Aiming in first person: the player is turned to the view for the first
// FP_AIM_TURN frames (the game would carry on turning them), the eye is held
// at their eyes (easing after them it would trail, walking or swinging, and
// look through the back of their head), and they are drawn (arms up, holding
// the gun or swinging the sword) from FP_AIM_SETTLE frames in, once the eye
// has caught up.
#define FP_AIM_TURN 15
#define FP_AIM_SETTLE 6
static int g_aim_frames, g_aim_settled;

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
#define TP_HEIGHT 180.0f
#define TP_DISTANCE 320.0f
// The right stick's up and down bring the eye closer or take it further
// (units a second at full stick), between these.
#define TP_ZOOM_RATE 400.0f
#define TP_CLOSEST 180.0f
#define TP_FARTHEST 800.0f
#define TP_PITCH 0.30f
// Its field of view (degrees, vertical): its own, not the shot's, so the
// view keeps its size through the game's cuts and its zooms.
#define TP_FOV 50.0f
// First person: the eye's height above the player's feet, and its field of
// view (degrees, vertical).
#define FP_HEIGHT 221.0f  // her eyes (measured: the top of her head is ~242)
#define FP_FOV 60.0f
// Its near clipping distance: past the hair round the eye (a ponytail swings
// forward through 30 with a sword).
#define FP_NEAR 45.0f
// The eye this far ahead of the head along the view.
#define FP_AHEAD 12.0f
// Aiming (the player drawn, swinging): further ahead, past their shoulders.
#define FP_AIM_AHEAD 12.0f
// The eyes below the top of the head (the skeleton's highest bone), kept
// within these heights.
#define FP_BELOW_TOP 25.0f
#define FP_MIN_EYE 180.0f
// Where only the root (the hips) is worked out, the eyes this many times its
// height (Alex: hips 129, eyes 221); a skeleton needs this many bones.
#define FP_EYE_PER_HIP 1.72f
#define FP_MIN_BONES 8
#define FP_MAX_EYE 235.0f
// How far ahead of the eye it looks (only the direction matters).
#define FP_LOOK 300.0f
// A sweep's length (seconds) and the share of it spent easing in and out.
#define SWEEP_TIME 0.8f
#define SWEEP_EASE 0.15f
// First person's handover to the game's view of a thing and back (seconds):
// one move: the eye on its way from the start while the view turns to the
// thing over the same time (the thing seen from where the eye is), so the
// view only rises a little over a thing above the eyes before settling on the
// game's framing; back, the same in reverse. The player is drawn (with the
// game's near plane) the whole way there and back, once the eye is
// FP_HEAD_CLEAR from their eyes.
// On the way the field of view keeps the thing growing (or, back, shrinking)
// steadily on screen, wherever the eye is: no zooming away and back in.
#define FP_HAND_TIME 1.4f
#define FP_HEAD_CLEAR 30.0f
#define FP_HAND_AIM 1.0f
#define FP_HAND_MOVE 0.0f
// Walls: the line test's radius, how far short of a wall the eye stops, and
// the nearest it comes to the point it looks at.
#define TP_RADIUS 12.0f
#define TP_MARGIN 30.0f
#define TP_NEAREST 120.0f
// The second line of sight, to the player's hips (this far below the chest).
#define TP_HIPS 70.0f
// How far above the chest the camera turns about when fully close in.
#define TP_CLOSE_RISE 35.0f
// Room enough behind the player not to rise over their head.
#define TP_ROOMY (0.6f * g_distance)
// Swinging round something in the way: its fastest turn (radians a second),
// how quickly its speed may change (radians a second, a second) and how far
// past the edge
// of the obstacle it goes (radians); and coming in short of it (units a
// second at least).
#define SWING_RATE 1.6f
#define SWING_ACCEL 4.0f
// How far ahead (frames of the player's movement) it looks for something
// coming between them.
#define SWING_AHEAD 30.0f
#define SWING_MARGIN 0.17f
#define PULL_RATE 600.0f
// A rise over the player's head is kept this many frames before it may
// come down, and the eye's distance is the nearest of this many frames'
// readings.
#define LIFT_HOLD 45
// Following behind the player: from this speed (units a second), fully at
// the second, the turn behind them (per second of the angle left), and not
// past this angle (radians: walking toward the camera keeps it there).
#define FOLLOW_MIN_SPEED 40.0f
#define FOLLOW_FULL_SPEED 250.0f
#define FOLLOW_RATE 1.6f
#define FOLLOW_MAX_ANGLE 2.1f
#define REACH_WINDOW 20
// The drawn view's springs (per second: higher follows sooner) and top
// speeds (radians or units a second): the angle round the player, the same
// while the stick turns it, its height, its distance in and out, what it
// looks at, and the player it follows.
#define SMOOTH_YAW 4.5f
#define SMOOTH_YAW_MAX 1.6f
#define SMOOTH_STEER 14.0f
#define SMOOTH_RISE 2.5f
#define SMOOTH_RISE_MAX 0.5f
#define SMOOTH_DIST_IN 5.0f
#define SMOOTH_DIST_OUT 2.5f
#define SMOOTH_DIST_MAX 700.0f
#define SMOOTH_DIST_ACCEL 3000.0f
// More than this past the nearest thing in the way, the distance comes in
// on a much quicker spring.
#define CLIP_SLACK 40.0f
#define SMOOTH_DIST_CLIP 14.0f
#define SMOOTH_AIM 6.0f
#define SMOOTH_FOLLOW 14.0f

static int g_last_mode = -1;
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

static int g_looking_known = -1;  // camera_set_examining's looking
// Handing the view to the game's own camera while the player looks closely
// at something (B, or a close-up the game cuts to: a thing it zooms in on, a
// hidden player), and back: progress 0..1, and its eased share of the view.
static float g_hand_progress, g_hand;
// The game's view as the look ends, eased back from (the game's own camera
// pulls back out to its shot then, which would swing the view out and in).
static bool g_handing;
static float g_fp_hand_fov;  // first person's field of view on the way (0: none)
static bool g_fp_hand_decided, g_fp_hand_near;
#define FP_HAND_NEAR 150.0f  // a thing this close to the eye: turn to the game's view
static Vec3 g_held_eye, g_held_target;
static float g_held_fov;

void camera_set_examining(bool hidden, int looking) {
    g_examining = hidden;
    g_looking_known = looking;
}

static float g_aim_blend;  // 0..1, eased in while aiming in first person (tests: ED_CAM_AIM_FOV, ED_CAM_AIM_DROP)

static float env_float(const char* name, float fallback) {
    const char* v = getenv(name);
    return v != NULL ? (float)atof(v) : fallback;
}

static float fp_fov(void) {
    static float aim = -1.0f;
    if (aim < 0.0f)
        aim = env_float("ED_CAM_AIM_FOV", FP_FOV);
    return FP_FOV + (aim - FP_FOV) * g_aim_blend;
}

float camera_field_of_view(float game_fov) {
    if (!g_view_active)
        return game_fov;
    const float own =
        g_last_mode == CAMERA_THIRD_PERSON ? TP_FOV : g_last_mode == CAMERA_FIRST_PERSON ? fp_fov() : game_fov;
    if (g_held_fov <= 0.0f)
        g_held_fov = game_fov;  // else held with the eye (camera_look_at)
    if (g_last_mode == CAMERA_FIRST_PERSON && g_fp_hand_fov > 0.0f)
        return g_fp_hand_fov;
    return own + (g_held_fov - own) * g_hand;
}

int camera_player_drawn(void);

float camera_near_plane(float game_near, unsigned long long frame) {
    static float near = -1.0f;  // ED_CAM_FP_NEAR (tests)
    if (near < 0.0f)
        near = getenv("ED_CAM_FP_NEAR") != NULL ? (float)atof(getenv("ED_CAM_FP_NEAR")) : FP_NEAR;
    // The game sets it before the frame's draws, ahead of the camera: aiming,
    // it moves a frame before the player is drawn (else that frame looks out
    // from inside their head).
    // Chosen once a frame: the game makes its projection twice a frame, either
    // side of the camera's step, and a change between the two drew part of
    // the frame (the basins' flames) with each.
    static unsigned long long latched = ~0ull;
    static bool ours;
    if (frame != latched) {
        latched = frame;
        const bool soon = g_aiming && g_aim_settled >= FP_AIM_SETTLE - 1;
        ours = g_view_active && g_last_mode == CAMERA_FIRST_PERSON && g_hand_progress <= 0.0f &&
               (camera_player_drawn() == 1 || soon);
    }
    return ours ? fmaxf(game_near, near) : game_near;
}

int camera_player_drawn(void) {
    if (!g_view_active)
        return -1;
    static int body = -1;  // ED_CAM_FP_BODY=0/1 (tests)
    if (body < 0)
        body = getenv("ED_CAM_FP_BODY") != NULL ? atoi(getenv("ED_CAM_FP_BODY")) != 0 : 0;
    // Mostly the game's view: the player as the game has them.
    if (g_last_mode == CAMERA_FIRST_PERSON) {
        // While the eye is out of their head, both ways: inside it the hair
        // and face flip across the view; any further out and they pop in or
        // out while in view.
        // Drawn even where the game hides them (its text boxes and close
        // looks hide them, its camera being at their head; ours is not).
        if (g_hand_progress > 0.0f && length(sub(g_view_eye, g_fp_eye)) >= FP_HEAD_CLEAR)
            return 1;
        // Aiming they stand still with their arms up: drawn, with the near
        // plane past their head, they hold the gun in front of the view.
        if (g_aiming && g_hand_progress <= 0.0f && g_aim_settled >= FP_AIM_SETTLE)
            return 1;
    } else if (g_last_mode == CAMERA_THIRD_PERSON) {
        // Always, as first person on the way: only the game's own close look
        // from their head (where it hides them) is left to the game.
        if (g_hand_progress > 0.0f && length(sub(g_view_eye, g_head_eye)) < FP_HEAD_CLEAR)
            return -1;
        return 1;
    }
    return g_last_mode == CAMERA_FIRST_PERSON ? body : -1;
}

static bool g_state_pending;  // camera_set_state: applied at the next frame
static float g_pending_state[CAMERA_STATE_SIZE];

void camera_get_state(float out[CAMERA_STATE_SIZE]) {
    const float s[CAMERA_STATE_SIZE] = {g_back.x, g_back.y, g_back.z, g_rise, g_lift, g_distance, g_reach, 0.0f};
    memcpy(out, s, sizeof s);
}

void camera_set_state(const float in[CAMERA_STATE_SIZE]) {
    memcpy(g_pending_state, in, sizeof g_pending_state);
    g_state_pending = true;
}

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

/* The player's turn: a quaternion about the up axis at player +44 (x, y, z,
 * w); they face (and walk) along it, a quarter turn round. */
#define PLAYER_TURN 44u

static bool player_facing(CPUState* cpu, Vec3 k, Vec3* dir) {
    if (g_player_offset == 0)
        return false;
    const u32 object = mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset);
    if (object < 0x80000000u || object - 0x80000000u > cpu->ram_size - 64u)
        return false;
    const float qx = read_float(cpu, object + PLAYER_TURN), qy = read_float(cpu, object + PLAYER_TURN + 4u),
                qz = read_float(cpu, object + PLAYER_TURN + 8u), qw = read_float(cpu, object + PLAYER_TURN + 12u);
    if (!(fabsf(qx) < 0.01f && fabsf(qy) < 0.01f && fabsf(qz * qz + qw * qw - 1.0f) < 0.05f))
        return false;
    const float facing = 2.0f * atan2f(qz, qw) - 0.5f * PI;
    *dir = normalize(flatten(v3(cosf(facing), sinf(facing), 0.0f), k));
    return length(*dir) > 0.5f;
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

/* How far out from the pivot along the unit direction out the eye can go:
 * the game's own line test (a little wider than the camera), stopping short
 * of the first wall, floor or ceiling in the way. */
static Vec3 g_up = {0.0f, 0.0f, 1.0f};  // the world's up, for the hip test

static float line_reach(CPUState* cpu, Vec3 from_point, Vec3 eye, float full) {
    const float from[3] = {from_point.x, from_point.y, from_point.z}, to[3] = {eye.x, eye.y, eye.z};
    float hit[3];
    if (!g_collide(cpu, from, to, TP_RADIUS, hit))
        return full;
    return length(sub(v3(hit[0], hit[1], hit[2]), from_point)) / length(sub(eye, from_point)) * full;
}

static float wall_reach(CPUState* cpu, Vec3 pivot, Vec3 out) {
    if (g_collide == NULL)
        return g_distance;
    // The line from the eye to the player's chest, and to their hips (a
    // cabinet below the first can still hide most of them).
    const float full = g_distance + TP_MARGIN;
    const Vec3 far = add(pivot, scale(out, full));
    const float chest = line_reach(cpu, pivot, far, full);
    const float hips = line_reach(cpu, sub(pivot, scale(g_up, TP_HIPS)), far, full);
    return clampf(fminf(chest, hips) - TP_MARGIN, TP_NEAREST, g_distance);
}

/* A sweep's progress (0..1 of its time) to how far along the line it is:
 * even speed, easing in and out over SWEEP_EASE of the time at each end. */
/* 0..1 eased in and out with no jolt at either end (smootherstep). */
static float smoother(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* From one direction to another (unit vectors), turning evenly. */
static Vec3 turn_towards(Vec3 a, Vec3 b, float t) {
    const float c = clampf(dot(a, b), -1.0f, 1.0f), angle = acosf(c);
    if (angle < 1e-4f)
        return b;
    const float sn = sinf(angle);
    if (sn < 1e-4f)
        return normalize(add(scale(a, 1.0f - t), scale(b, t)));
    return add(scale(a, sinf((1.0f - t) * angle) / sn), scale(b, sinf(t * angle) / sn));
}

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

/* Once a frame: what the camera looks at, the player, swept in a straight
 * line from wherever it looked (a reset, a jump). */
static void sweep_aim(Vec3 focus, Vec3 pivot, Vec3 k, bool jumped) {
    // Looking closely at something now hands the view to the game's own
    // camera (camera_look_at), so the aim stays on the player.
    const bool looking = false;
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

// 0 .. 1: how badly the drawn view is blocked (1: only the nearest distance
// is clear); the swing and the drawn turn may go up to twice as fast.
static float g_urgency;

/* The swing round something in the way: where it is going (a level
 * direction from the player to the eye) and how fast it turns (radians a
 * second, about the up axis). */
static bool g_swing_active;
static Vec3 g_swing_goal;
static float g_swing_speed;

static Vec3 out_dir(Vec3 back, Vec3 k, float rise) { return add(scale(back, cosf(rise)), scale(k, sinf(rise))); }

/* Room enough along dir from the player now and where they will be in half
 * a second and a second. */
static bool clear_along(CPUState* cpu, Vec3 pivot, Vec3 step, Vec3 dir) {
    for (int i = 0; i <= 2; i++)
        if (wall_reach(cpu, add(pivot, scale(step, (float)i)), dir) < TP_ROOMY)
            return false;
    return true;
}

static void auto_swing(CPUState* cpu, Vec3 pivot, Vec3 step, Vec3 k, float rise, bool hold) {
    static int blocked_frames;
    if (g_collide == NULL || hold) {
        g_swing_active = false;
        g_swing_speed = 0.0f;
        blocked_frames = 0;
        return;
    }
    // Blocked now or soon (where the player is heading), for a few frames
    // running: starting early leaves time to swing smoothly instead of
    // closing in, and a few frames keep it from hunting.
    blocked_frames = clear_along(cpu, pivot, step, out_dir(g_back, k, rise)) ? 0 : blocked_frames + 1;
    const bool blocked_here = blocked_frames >= 4;
    const bool goal_blocked = g_swing_active && wall_reach(cpu, pivot, out_dir(g_swing_goal, k, rise)) < TP_ROOMY;
    if ((blocked_here && !g_swing_active) || goal_blocked) {
        // The nearest side with room, either way (the turn's speed changes
        // gently, so turning back is smooth too); its own way on a tie.
        const bool was_swinging = g_swing_active;
        g_swing_active = false;
        const int way = g_swing_speed < 0.0f ? -1 : 1;
        for (int n = 0; n < 36 && !g_swing_active; n++) {
            const int i = n / 2 + 1;
            const int side = n % 2 == 0 ? way : -way;
            const float angle = (float)(side * i) * PI / 36.0f;
            if (!clear_along(cpu, pivot, step, out_dir(rotate(g_back, k, angle), k, rise)))
                continue;
            // A little past the edge, where that is clear too.
            const float past = angle + (float)side * SWING_MARGIN;
            const Vec3 beyond = rotate(g_back, k, past);
            g_swing_goal = wall_reach(cpu, pivot, out_dir(beyond, k, rise)) >= TP_ROOMY ? beyond
                                                                                         : rotate(g_back, k, angle);
            g_swing_active = true;
        }
        // Nowhere with room: a swing under way slows to a stop where it is.
        if (!g_swing_active && was_swinging) {
            g_swing_goal = g_back;
            g_swing_active = true;
        }
    }
    if (!g_swing_active)
        return;
    // Clear where it is but blocked just ahead on its way: the way there
    // runs behind something. It eases to a stop here instead.
    if (!blocked_here && fabsf(g_swing_speed) > 0.01f) {
        const Vec3 next = rotate(g_back, k, copysignf(0.26f, g_swing_speed));
        if (!clear_along(cpu, pivot, step, out_dir(next, k, rise)) &&
            clear_along(cpu, pivot, step, out_dir(g_back, k, rise)))
            g_swing_goal = g_back;
    }
    // The turn's speed changes by at most SWING_ACCEL a second each second,
    // aiming for the fastest speed it can still stop from in the angle left:
    // it eases into the turn and out of it, and a new goal bends it smoothly.
    const float dt = 1.0f / 60.0f;
    const float left = level_angle(g_back, g_swing_goal, k);
    const float boost = 1.0f + g_urgency, accel = SWING_ACCEL * boost;
    const float want = copysignf(fminf(SWING_RATE * boost, sqrtf(2.0f * accel * fabsf(left))), left);
    g_swing_speed += clampf(want - g_swing_speed, -accel * dt, accel * dt);
    g_back = normalize(rotate(g_back, k, g_swing_speed * dt));
    if (fabsf(left) < 0.005f && fabsf(g_swing_speed) < 0.05f) {
        g_swing_active = false;
        g_swing_speed = 0.0f;
    }
    static int log = -1;
    if (log < 0)
        log = getenv("ED_CAM_LOG") != NULL && getenv("ED_CAM_LOG")[0] == '4';
    if (log)
        fprintf(stderr, "[camera] swing left %.1f deg, speed %.1f deg/s\n", left * 57.2958f, g_swing_speed * 57.2958f);
}

/* The view as drawn: whatever the logic above decides (where round the
 * player, how high, how far, what it looks at), the camera follows it on
 * critically damped springs, its speeds capped, so it never jumps: the angle
 * round the player, the height and the distance in its own coordinates (an
 * orbit stays an orbit), and what it looks at relative to the player (no lag
 * while walking). The stick's own turning follows closely. */
static bool g_view_jump, g_smooth_reset = true;
static float g_s_yaw, g_s_yaw_v, g_s_rise, g_s_rise_v, g_s_dist, g_s_dist_v;
static Vec3 g_s_aim, g_s_aim_v, g_s_pivot, g_s_pivot_v;

static Vec3 yaw_axis(Vec3 k) {
    Vec3 e1 = normalize(flatten(v3(1.0f, 0.0f, 0.0f), k));
    if (length(e1) == 0.0f)
        e1 = normalize(flatten(v3(0.0f, 1.0f, 0.0f), k));
    return e1;
}

static float dir_yaw(Vec3 b, Vec3 k) {
    const Vec3 e1 = yaw_axis(k), e2 = cross(k, e1);
    return atan2f(dot(b, e2), dot(b, e1));
}

static Vec3 yaw_dir(float yaw, Vec3 k) {
    const Vec3 e1 = yaw_axis(k), e2 = cross(k, e1);
    return add(scale(e1, cosf(yaw)), scale(e2, sinf(yaw)));
}

static void spring_capped(float* x, float* v, float goal, float w, float cap, float accel) {
    const float dt = 1.0f / 60.0f;
    *v += clampf(w * w * (goal - *x) - 2.0f * w * *v, -accel, accel) * dt;
    *v = clampf(*v, -cap, cap);
    *x += *v * dt;
}

static void spring(float* x, float* v, float goal, float w, float cap) {
    const float dt = 1.0f / 60.0f;
    *v += (w * w * (goal - *x) - 2.0f * w * *v) * dt;
    *v = clampf(*v, -cap, cap);
    *x += *v * dt;
}

static void smooth_view(Vec3 pivot, Vec3 k, bool reset) {
    const float yaw = dir_yaw(g_back, k), rise = g_rise + g_lift;
    const Vec3 aim = sub(g_aim, pivot);
    if (reset) {
        g_s_yaw = yaw, g_s_rise = rise, g_s_dist = g_reach, g_s_aim = aim, g_s_pivot = pivot;
        g_s_yaw_v = g_s_rise_v = g_s_dist_v = 0.0f;
        g_s_aim_v = g_s_pivot_v = v3(0.0f, 0.0f, 0.0f);
        return;
    }
    // The player stopping short against a wall, or turning on the spot,
    // is a jolt in their own movement: the camera follows them closely
    // rather than rigidly.
    float px = g_s_pivot.x, py = g_s_pivot.y, pz = g_s_pivot.z;
    spring(&px, &g_s_pivot_v.x, pivot.x, SMOOTH_FOLLOW, 3000.0f);
    spring(&py, &g_s_pivot_v.y, pivot.y, SMOOTH_FOLLOW, 3000.0f);
    spring(&pz, &g_s_pivot_v.z, pivot.z, SMOOTH_FOLLOW, 3000.0f);
    g_s_pivot = v3(px, py, pz);
    const bool steering = fabsf(g_stick_x) >= 0.1f;
    // The angle the short way round.
    const float goal_yaw = g_s_yaw + remainderf(yaw - g_s_yaw, 2.0f * PI);
    spring(&g_s_yaw, &g_s_yaw_v, goal_yaw, steering ? SMOOTH_STEER : SMOOTH_YAW * (1.0f + g_urgency),
           steering ? 4.0f : SMOOTH_YAW_MAX * (1.0f + g_urgency));
    g_s_yaw = remainderf(g_s_yaw, 2.0f * PI);
    spring(&g_s_rise, &g_s_rise_v, rise, SMOOTH_RISE, SMOOTH_RISE_MAX);
    // In a little quicker than out: something in the way matters more.
    // Well past something in the way (inside a cabinet, behind a pillar)
    // it comes in quickly: smooth is good, but not through furniture.
    if (g_s_dist > g_reach + CLIP_SLACK)
        spring_capped(&g_s_dist, &g_s_dist_v, g_reach, SMOOTH_DIST_CLIP, 3000.0f, 40000.0f);
    else
        spring_capped(&g_s_dist, &g_s_dist_v, g_reach, g_reach < g_s_dist ? SMOOTH_DIST_IN : SMOOTH_DIST_OUT,
                      SMOOTH_DIST_MAX, SMOOTH_DIST_ACCEL);
    float ax = g_s_aim.x, ay = g_s_aim.y, az = g_s_aim.z;
    spring(&ax, &g_s_aim_v.x, aim.x, SMOOTH_AIM, 2000.0f);
    spring(&ay, &g_s_aim_v.y, aim.y, SMOOTH_AIM, 2000.0f);
    spring(&az, &g_s_aim_v.z, aim.z, SMOOTH_AIM, 2000.0f);
    g_s_aim = v3(ax, ay, az);
}

/* Third person: behind and above the player, trailing after them. Starts
 * from the side the game's camera is on. While the player looks closely at
 * something, the game's camera looks at it (focus): this one turns to it too,
 * from behind the player, so a thing high on a wall comes into view. */
static void third_person(CPUState* cpu, Vec3 eye, Vec3 focus, Vec3 player, Vec3 k, bool step, bool still,
                         Vec3* new_eye, Vec3* new_target) {
    const Vec3 pivot = add(player, scale(k, TP_HEIGHT));
    g_up = k;
    if (step && g_state_pending) {
        // A replay's start: the camera as it was when the recording began.
        g_state_pending = false;
        g_back = v3(g_pending_state[0], g_pending_state[1], g_pending_state[2]);
        g_rise = g_pending_state[3];
        g_lift = g_pending_state[4];
        g_distance = g_pending_state[5];
        g_reach = g_pending_state[6];
        g_last_player = player;
        g_have_player = true;
        g_smooth_reset = true;
    }
    if (step) {
        const bool jumped = !g_have_player || length(sub(player, g_last_player)) > 300.0f;
        g_view_jump = jumped || g_smooth_reset;
        g_smooth_reset = false;
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
            // Follow behind: while the player moves, the camera comes round
            // behind the way they are going, the faster they move the
            // sooner, so walking round something keeps it out of the way
            // (the floor they came over is open). Not when they walk toward
            // the camera (it keeps their face), nor while the stick turns it
            // or it swings round something or to a thing looked at.
            const Vec3 moved = flatten(sub(player, g_last_player), k);
            const float speed = length(moved) * 60.0f;
            if (speed > FOLLOW_MIN_SPEED && fabsf(g_stick_x) < 0.1f && !g_swing_active && !g_back_sweeps) {
                const Vec3 behind = normalize(scale(moved, -1.0f));
                const float angle = level_angle(g_back, behind, k);
                if (fabsf(angle) < FOLLOW_MAX_ANGLE) {
                    const float rate = FOLLOW_RATE * fminf(1.0f, speed / FOLLOW_FULL_SPEED);
                    g_back = normalize(rotate(g_back, k, clampf(angle * rate * dt, -fabsf(angle), fabsf(angle))));
                }
            }
            g_distance = clampf(g_distance - g_stick_y * TP_ZOOM_RATE * dt, TP_CLOSEST, TP_FARTHEST);
        }
        // Something between the player and the eye (a cabinet, a pillar):
        // pick the nearest side with room (5 degree steps either way, up to
        // 90, and a little past the edge) and swing there, easing in and out
        // of the turn. Not while the stick turns the camera,
        // the player aims, or it sweeps to a thing; a new side is only looked
        // for when the one it is swinging to is blocked too.
        const float rise_now = g_rise + g_lift;
        const Vec3 step = jumped ? v3(0.0f, 0.0f, 0.0f) : scale(sub(player, g_last_player), SWING_AHEAD);
        auto_swing(cpu, pivot, step, k, rise_now, jumped || still || g_back_sweeps || fabsf(g_stick_x) >= 0.1f);
        // Walls are measured along the direction the camera is drawn in (it
        // eases after where it is going), so the drawn eye is always in front
        // of what is in the way.
        const Vec3 drawn = g_view_jump ? g_back : yaw_dir(g_s_yaw, k);
        // Room enough there, or it rises over the player's head (looking
        // down on them, over a cabinet) to where there is more.
        float best = -1.0f, lift = 0.0f;
        for (int i = 0; i < 3 && g_collide != NULL; i++) {
            const float rise = fminf(g_rise + 0.3f * (float)i, 1.35f);
            const float reach = wall_reach(cpu, pivot, add(scale(drawn, cosf(rise)), scale(k, sinf(rise))));
            if (reach > best + 1.0f) {
                best = reach;
                lift = rise - g_rise;
            }
            if (reach >= TP_ROOMY)
                break;
        }
        // Once it has had to rise, it stays up a while: a rise that came and
        // went with every glance of the line off the furniture was the
        // up-and-down judder.
        static int lift_hold;
        if (jumped || lift >= g_lift) {
            g_lift = lift;
            lift_hold = LIFT_HOLD;
        } else if (lift_hold > 0) {
            lift_hold--;
        } else {
            g_lift = lift;
        }
        // Then how far out the eye can go: the nearest of the last few
        // frames' readings, so a line glancing on and off an edge does not
        // pump it in and out.
        const float rise = g_rise + g_lift;
        const float reach = wall_reach(cpu, pivot, add(scale(drawn, cosf(rise)), scale(k, sinf(rise))));
        static float window[REACH_WINDOW];
        static int window_at, window_n;
        if (jumped)
            window_n = 0;
        window[window_at] = reach;
        window_at = (window_at + 1) % REACH_WINDOW;
        if (window_n < REACH_WINDOW)
            window_n++;
        g_reach = reach;
        for (int i = 0; i < window_n; i++)
            g_reach = fminf(g_reach, window[i]);
        // Eased in and out itself: a sudden change of pace is a jolt too.
        const float urgent = clampf((g_distance * 0.6f - reach) / (g_distance * 0.6f - TP_NEAREST), 0.0f, 1.0f);
        g_urgency += clampf(urgent - g_urgency, -1.0f / 60.0f, 2.0f / 60.0f);
        static int log = -1;
        if (log < 0)
            log = getenv("ED_CAM_LOG") != NULL && getenv("ED_CAM_LOG")[0] == '4';
        if (log) {
            fprintf(stderr, "[camera] reach %.0f (wall %.0f) lift %.2f player %.0f %.0f frame %llu ring", g_reach, reach,
                    g_lift, player.x, player.y, (unsigned long long)(cpu->timebase / 675000ull));
            for (int i = 0; i < 16; i++) {
                const Vec3 b = rotate(g_back, k, (float)i * PI / 8.0f);
                fprintf(stderr, " %.0f", wall_reach(cpu, pivot, add(scale(b, cosf(g_rise)), scale(k, sinf(g_rise)))));
            }
            fprintf(stderr, "\n");
        }
        g_last_player = player;
        g_have_player = true;
    }
    if (step) {
        smooth_view(pivot, k, g_view_jump);
        static int log = -1;
        if (log < 0)
            log = getenv("ED_CAM_LOG") != NULL && getenv("ED_CAM_LOG")[0] == '4';
        if (log && g_collide != NULL) {
            // Can the drawn eye see the player? (for tuning: ED_CAM_LOG=4)
            const Vec3 eye_now = add(g_s_pivot, scale(out_dir(yaw_dir(g_s_yaw, k), k, g_s_rise), g_s_dist));
            const float chest = line_reach(cpu, pivot, eye_now, 1.0f);
            const float hips = line_reach(cpu, sub(pivot, scale(k, TP_HIPS)), eye_now, 1.0f);
            fprintf(stderr, "[camera] seen chest %.2f hips %.2f dist %.0f\n", chest, hips, g_s_dist);
        }
    }
    // Close in, it frames the player's head and shoulders rather than their
    // middle: the point it turns about rises toward head height.
    const float close = clampf(1.0f - (g_s_dist - TP_NEAREST) / fmaxf(1.0f, g_distance - TP_NEAREST), 0.0f, 1.0f);
    const Vec3 centre = add(g_s_pivot, scale(k, close * TP_CLOSE_RISE));
    *new_eye = add(centre, scale(out_dir(yaw_dir(g_s_yaw, k), k, g_s_rise), g_s_dist));
    *new_target = add(centre, g_s_aim);
}

/* The player's head, from their skeleton. The game works out each bone of a
 * character (fn(character, bone, out)) and, as it goes, each bone's place
 * relative to the character's feet into a scratch block, bone i at the
 * root's out + 48 * i, character after character. The player's block is
 * read once theirs is done (another character's begin, or at the camera if
 * theirs came last): the highest bone is the top of the head (or helmet),
 * with where it leans. Alex: 246; the eyes FP_BELOW_TOP under it. Some
 * rooms work out only the root (the hips) this way: the eyes are then
 * FP_EYE_PER_HIP times its height. */
#define MAX_BONES 128
static CPUState* g_cpu;  // the game's CPU, for the camera's own reads
static u32 g_bone_block;  // the player's (0: none pending)
static int g_bone_n;
static bool g_head_valid;
static Vec3 g_head_top;  // from the player's position
static float g_eye_height;  // above their feet

static void read_player_bones(CPUState* cpu) {
    const u32 block = g_bone_block;
    const int n = g_bone_n;
    g_bone_block = 0u;
    g_bone_n = 0;
    if (block < 0x80000000u || block - 0x80000000u > cpu->ram_size - (u32)n * 48u || n <= 0)
        return;
    float best = -1e9f;
    Vec3 top = v3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < n; i++) {
        const u32 m = block + (u32)i * 48u;
        const Vec3 t = v3(read_float(cpu, m + 12u), read_float(cpu, m + 28u), read_float(cpu, m + 44u));
        if (isfinite(t.x) && isfinite(t.y) && isfinite(t.z) && fabsf(t.x) < 200.0f && fabsf(t.y) < 200.0f &&
            t.z > best)
            best = t.z, top = t;
    }
    if (best > 100.0f && best < 500.0f && n >= FP_MIN_BONES) {
        g_head_top = top;
        g_eye_height = best - FP_BELOW_TOP;
        g_head_valid = true;
    } else if (best > 50.0f && best < 300.0f && n < FP_MIN_BONES) {
        g_head_top = top;
        g_eye_height = read_float(cpu, block + 44u) * FP_EYE_PER_HIP;
        g_head_valid = true;
    }
}

void camera_bone_matrix(CPUState* cpu, unsigned character, unsigned bone, unsigned out) {
    static u32 last_character;
    const u32 player = g_player_offset != 0 ? mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset) : 0u;
    if (character != last_character && g_bone_block != 0u)
        read_player_bones(cpu);  // the player's are done, still in place
    last_character = character;
    if (character != player || player == 0u || bone >= MAX_BONES)
        return;
    // Single bones are also asked for elsewhere (where a sword hangs), into
    // the stack: only the root's out off the stack starts the block.
    const bool on_stack = out + 0x4000u > cpu->gpr[1] && out < cpu->gpr[1] + 0x4000u;
    if (bone == 0u && !on_stack) {
        g_bone_block = out;
        static int log = -1;
        if (log < 0)
            log = getenv("ED_CAM_LOG") != NULL && getenv("ED_CAM_LOG")[0] == '5';
        if (log)
            fprintf(stderr, "[camera] player 0x%08X bones from 0x%08X\n", character, out);
    }
    if (!on_stack && (int)bone + 1 > g_bone_n)
        g_bone_n = (int)bone + 1;
}

/* Aiming in first person the player is drawn for their arms and what they
 * hold, but not their head, chest or legs (a helmet's crest, a shoulder plate
 * rising into the view as they swing). The game works out each character's
 * bones into the scratch block, then skins its mesh from them (hle.c steps in
 * around that, for the player): every bone but the arms' has its turn set to
 * nothing there, so what hangs off it shrinks to a point, and gets it back
 * straight after. The arms are found in the skeleton (model +36: 80 bytes a
 * bone, its parent's at +60): the branches of the bone with the most of
 * them that end in a hand (a bone with three or more children, its fingers). */
#define SKIN_MAX_BONES 128
// Kept: the hand, its fingers and what it holds, and this many bones above it
// (forearm and upper arm; above them a shoulder plate hangs off the
// collarbone, and with only the forearm the upper arm stretches to points).
#define FP_ARM_BONES 2
static u32 g_skin_model;
static bool g_skin_keep[SKIN_MAX_BONES];
static int g_skin_bones;
static float g_skin_saved[SKIN_MAX_BONES][3];
static bool g_skin_collapsed;

static void skin_find_arms(CPUState* cpu, u32 model, int n) {
    g_skin_model = model;
    g_skin_bones = 0;
    const u32 table = mem_read32(cpu, model + 36u);
    if (table < 0x80000000u || table - 0x80000000u > cpu->ram_size - (u32)n * 80u || n > SKIN_MAX_BONES)
        return;
    int parent[SKIN_MAX_BONES], kids[SKIN_MAX_BONES] = {0};
    for (int i = 0; i < n; i++) {
        const u32 p = mem_read32(cpu, table + (u32)i * 80u + 60u);
        parent[i] = p >= table && (p - table) % 80u == 0u && (int)((p - table) / 80u) < n ? (int)((p - table) / 80u) : -1;
        if (parent[i] >= 0)
            kids[parent[i]]++;
    }
    // Hands: three or more children. The chest: the nearest bone above two
    // hands that both pass through.
    int hands[8], h = 0;
    for (int i = 0; i < n && h < 8; i++) {
        if (kids[i] < 3 || parent[i] < 0)
            continue;
        // A hand's children are its fingers: each a short chain, unbranched.
        bool fingers = true;
        for (int j = 0; j < n && fingers; j++) {
            if (parent[j] != i)
                continue;
            int length = 1;
            for (int c = j; kids[c] == 1 && length <= 4; length++) {
                int next = -1;
                for (int x = 0; x < n; x++)
                    if (parent[x] == c)
                        next = x;
                c = next;
            }
            int end = j;  // where the chain stops: a leaf, or it branches
            for (int step = 0; step < length - 1; step++)
                for (int x = 0; x < n; x++)
                    if (parent[x] == end) {
                        end = x;
                        break;
                    }
            fingers = length <= 4 && kids[end] == 0;
        }
        if (fingers)
            hands[h++] = i;
    }
    if (h < 1)
        return;
    // Each hand's arm: up from the hand to the first bone with more than one
    // child (the chest), every bone on the way and below it kept.
    for (int i = 0; i < n; i++)
        g_skin_keep[i] = false;
    int arms = 0;
    static int up = -1;  // ED_CAM_ARM_BONES: bones kept above the hand (tests)
    if (up < 0)
        up = getenv("ED_CAM_ARM_BONES") != NULL ? atoi(getenv("ED_CAM_ARM_BONES")) : FP_ARM_BONES;
    for (int a = 0; a < h; a++) {
        int top = hands[a];
        for (int k = 0; k < up && parent[top] >= 0 && kids[parent[top]] == 1; k++)
            top = parent[top];
        if (parent[top] < 0)
            continue;
        for (int i = 0; i < n; i++)  // the arm's branch: top and all below it
            for (int j = i; j >= 0; j = parent[j])
                if (j == top) {
                    g_skin_keep[i] = true;
                    break;
                }
        arms++;
    }
    if (arms > 0)
        g_skin_bones = n;
    if (getenv("ED_CAM_LOG") != NULL)
        fprintf(stderr, "[camera] first person aiming: %d arms in %d bones\n", arms, n);
}

static u32 g_skin_table;

/* The skeleton's bones: its table's entries while each looks like one (its
 * scale's three floats near 1 or 0, its parent in the table or none). */
static int skin_count_bones(CPUState* cpu, u32 table) {
    int n = 0;
    while (n < SKIN_MAX_BONES) {
        const u32 e = table + (u32)n * 80u;
        if (e - 0x80000000u > cpu->ram_size - 80u)
            break;
        bool ok = true;
        for (int c = 0; c < 3 && ok; c++) {
            const float v = read_float(cpu, e + (u32)c * 4u);
            ok = isfinite(v) && v >= 0.0f && v < 4.0f;
        }
        const u32 p = mem_read32(cpu, e + 60u);
        ok = ok && (p == 0u || (p >= table && p < e && (p - table) % 80u == 0u));
        if (!ok)
            break;
        n++;
    }
    return n;
}

void camera_skin_begin(CPUState* cpu, unsigned character) {
    g_skin_collapsed = false;
    const u32 player = g_player_offset != 0 ? mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset) : 0u;
    if (character != player || player == 0u)
        return;
    static int everywhere = -1;  // ED_CAM_ARMS_ONLY=1: in every mode, always (tests)
    if (everywhere < 0)
        everywhere = getenv("ED_CAM_ARMS_ONLY") != NULL;
    if (!everywhere && !(g_view_active && g_last_mode == CAMERA_FIRST_PERSON && g_aiming && g_hand_progress <= 0.0f))
        return;
    const u32 model = mem_read32(cpu, character + 60u);
    if (model < 0x80000000u || model - 0x80000000u > cpu->ram_size - 64u)
        return;
    if (model != g_skin_model) {
        const u32 table = mem_read32(cpu, model + 36u);
        const int n = table >= 0x80000000u && table - 0x80000000u < cpu->ram_size ? skin_count_bones(cpu, table) : 0;
        skin_find_arms(cpu, model, n);
    }
    if (g_skin_bones <= 0)
        return;
    g_skin_table = mem_read32(cpu, model + 36u);
    static int all = -1;
    if (all < 0)
        all = getenv("ED_SKIN_ALL") != NULL;
    for (int i = 0; i < g_skin_bones; i++) {
        if (g_skin_keep[i] && !all)
            continue;
        const u32 e = g_skin_table + (u32)i * 80u;
        for (int c = 0; c < 3; c++) {
            g_skin_saved[i][c] = read_float(cpu, e + (u32)c * 4u);
            mem_write32(cpu, e + (u32)c * 4u, 0u);
        }
    }
    g_skin_collapsed = true;
}

void camera_skin_end(CPUState* cpu, unsigned character) {
    (void)character;
    if (!g_skin_collapsed)
        return;
    g_skin_collapsed = false;
    static int all = -1;
    if (all < 0)
        all = getenv("ED_SKIN_ALL") != NULL;
    for (int i = 0; i < g_skin_bones; i++) {
        if (g_skin_keep[i] && !all)
            continue;
        const u32 e = g_skin_table + (u32)i * 80u;
        for (int c = 0; c < 3; c++) {
            u32 bits;
            memcpy(&bits, &g_skin_saved[i][c], sizeof bits);
            mem_write32(cpu, e + (u32)c * 4u, bits);
        }
    }
}

/* First person: the eye at the player's head (at their own height, from
 * their skeleton), turned with the right stick: across, and up and down. The
 * player is not drawn: their head, hair and helmet swing well forward of
 * where they stand as they walk (ED_CAM_FP_BODY=1 draws them, with the near
 * clipping distance past their hair). It starts looking where the
 * game's camera looks; when the player looks closely at something it sweeps
 * to it and back, like third person. The eye follows the player on the same
 * spring as third person's (a step or a stop does not jolt it). */
static float g_fp_yaw, g_fp_pitch;

static void first_person(Vec3 eye, Vec3 target, Vec3 focus, Vec3 player, Vec3 k, bool step, bool still,
                         Vec3* new_eye, Vec3* new_target) {
    // A little ahead of the head along the view.
    Vec3 head = add(add(player, scale(k, FP_HEIGHT)), scale(yaw_dir(g_fp_yaw, k), FP_AHEAD));
    // Better, at the player's own height, from their skeleton.
    static int log = -1;
    if (log < 0)
        log = getenv("ED_CAM_LOG") != NULL;
    if (step && g_bone_block != 0u)
        read_player_bones(g_cpu);  // the player's came last
    if (g_head_valid) {
        // Their own height, within FP_MIN_EYE..FP_MAX_EYE (a helmet's crest
        // or a ponytail is the top of some, and leans well off the face).
        static float eye_z = FP_HEIGHT;
        if (step)
            eye_z += (clampf(g_eye_height, FP_MIN_EYE, FP_MAX_EYE) - eye_z) * 0.1f;
        static float aim_ahead = -1.0f;  // ED_CAM_AIM_AHEAD (tests)
        if (aim_ahead < 0.0f)
            aim_ahead = getenv("ED_CAM_AIM_AHEAD") != NULL ? (float)atof(getenv("ED_CAM_AIM_AHEAD")) : FP_AIM_AHEAD;
        static float drop = -1.0f;
        if (drop < 0.0f)
            drop = env_float("ED_CAM_AIM_DROP", 0.0f);
        if (step)
            g_aim_blend = clampf(g_aim_blend + (still ? 1.0f : -1.0f) / 12.0f, 0.0f, 1.0f);
        head = add(add(player, scale(k, eye_z - drop * g_aim_blend)),
                   scale(yaw_dir(g_fp_yaw, k), still ? aim_ahead : FP_AHEAD));
        if (log && step) {
            static unsigned n;
            if (n++ % 30u == 0u)
                fprintf(stderr, "[camera] head top %.1f %.1f %.1f, eyes %.1f from the feet\n", g_head_top.x,
                        g_head_top.y, g_head_top.z, eye_z);
        }
    }
    if (step) {
        const bool jumped = !g_have_player || length(sub(player, g_last_player)) > 300.0f;
        if (jumped) {
            // Where the player faces (the game's camera can look across them,
            // through a door say), else where it looks.
            Vec3 facing;
            if (!player_facing(g_cpu, k, &facing))
                facing = normalize(flatten(sub(target, eye), k));
            g_fp_yaw = dir_yaw(facing, k);
            if (log)
                fprintf(stderr, "[camera] first person starts facing %.0f deg (the game's camera %.0f)\n",
                        g_fp_yaw * 180.0f / PI, dir_yaw(normalize(flatten(sub(target, eye), k)), k) * 180.0f / PI);
            g_fp_pitch = 0.0f;
            g_s_pivot = head;
            g_s_pivot_v = v3(0.0f, 0.0f, 0.0f);
        } else if (!still) {
            const float dt = 1.0f / 60.0f;
            g_fp_yaw = remainderf(g_fp_yaw - g_stick_x * YAW_RATE * dt, 2.0f * PI);
            g_fp_pitch = clampf(g_fp_pitch + g_stick_y * PITCH_RATE * dt, -1.2f, 1.2f);
        }
        float px = g_s_pivot.x, py = g_s_pivot.y, pz = g_s_pivot.z;
        spring(&px, &g_s_pivot_v.x, head.x, SMOOTH_FOLLOW, 3000.0f);
        spring(&py, &g_s_pivot_v.y, head.y, SMOOTH_FOLLOW, 3000.0f);
        spring(&pz, &g_s_pivot_v.z, head.z, SMOOTH_FOLLOW, 3000.0f);
        g_s_pivot = v3(px, py, pz);
        if (still) {
            // Aiming, the eye is at their eyes, not easing after them: a
            // sword's swing carries them forward, and a trailing eye would
            // look through the back of their head.
            g_s_pivot = head;
            g_s_pivot_v = v3(0.0f, 0.0f, 0.0f);
            g_aim_frames++;
            g_aim_settled = g_aim_frames;
        } else {
            g_aim_frames = g_aim_settled = 0;
        }
        // What it looks at: ahead of the eye, or swept to a thing looked at.
        const Vec3 ahead = add(g_s_pivot, scale(out_dir(yaw_dir(g_fp_yaw, k), k, g_fp_pitch), FP_LOOK));
        sweep_aim(focus, ahead, k, jumped);
        g_back_sweeps = false;  // nothing to turn round behind
        g_last_player = player;
        g_have_player = true;
    }
    g_fp_eye = g_s_pivot;
    *new_eye = g_s_pivot;
    *new_target = g_aim;
}

/* Facing where the left stick points at once. The game turns the player
 * towards the stick at its own rate (about 15 degrees a frame) and walks on
 * meanwhile, so turning back round is a U-turn of a fifth of a second. Their
 * turn is a quaternion about the up axis at player +44 (x, y, z, w; z, w at
 * +52, +56) and they walk along it, a quarter turn round: this sets it to the
 * stick's direction from the view each frame the stick is pushed, and the
 * game, already facing that way, just walks. Not while their turn leans off
 * the up axis (x or y set). */
#define FACE_MIN_STICK 0.25f
static float g_move_x, g_move_y;

void camera_set_move(float x, float y) {
    g_move_x = x;
    g_move_y = y;
}

/* Turns the player to face along dir (level). */
static void player_set_facing(CPUState* cpu, Vec3 dir) {
    if (g_player_offset == 0)
        return;
    const u32 object = mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset);
    if (object < 0x80000000u || object - 0x80000000u > cpu->ram_size - 64u)
        return;
    const float qx = read_float(cpu, object + PLAYER_TURN), qy = read_float(cpu, object + PLAYER_TURN + 4u),
                qz = read_float(cpu, object + PLAYER_TURN + 8u), qw = read_float(cpu, object + PLAYER_TURN + 12u);
    if (!(fabsf(qx) < 0.01f && fabsf(qy) < 0.01f && fabsf(qz * qz + qw * qw - 1.0f) < 0.05f))
        return;
    const float turn = atan2f(dir.y, dir.x) + 0.5f * PI;
    float z = sinf(0.5f * turn), w = cosf(0.5f * turn);
    if (z * qz + w * qw < 0.0f)
        z = -z, w = -w;  // the same turn, on the side it already is
    const float f[2] = {z, w};
    for (int i = 0; i < 2; i++) {
        u32 bits;
        memcpy(&bits, &f[i], sizeof bits);
        mem_write32(cpu, object + PLAYER_TURN + 8u + (u32)i * 4u, bits);
    }
}

static void face_stick(CPUState* cpu, Vec3 view, Vec3 k) {
    if (hypotf(g_move_x, g_move_y) < FACE_MIN_STICK)
        return;
    const Vec3 forward = normalize(flatten(view, k));
    player_set_facing(cpu, add(scale(forward, g_move_y), scale(cross(forward, k), g_move_x)));
}

static bool g_log_checked, g_log;

/* What the player holds (a sword, a gun) is drawn from a list beside their
 * character: the player object, a zero, then HELD_SLOTS of (thing, flags);
 * the game clears a thing's flags to hide it (its text boxes do), drawn while
 * HELD_DRAWN is set; a muzzle flash is one, shown as the gun fires. The camera sets them as it has the player drawn (none
 * in first person, all in third person and on the way to a thing), writing
 * the game's own back otherwise; a value the game wrote meanwhile is its new
 * own. The list is found near the player object whenever that changes. */
#define HELD_SLOTS 8
#define HELD_DRAWN 0x00010000u  // set while a thing is drawn (it fades out through 0x40000)
#define HELD_SHOWN 0x00050000u  // a drawn thing's flags, until the game's are seen
#define HELD_SEARCH 0x100000u
static u32 g_held_player, g_held_list;
static u32 g_held_own[HELD_SLOTS], g_held_written[HELD_SLOTS], g_held_shown[HELD_SLOTS];
static bool g_held_overridden;
static bool g_held_was_drawn[HELD_SLOTS];
static bool g_menu_open;  // camera_set_menu

void camera_set_menu(bool open) { g_menu_open = open; }

static bool held_list_at(CPUState* cpu, u32 at, u32 player) {
    if (mem_read32(cpu, at) != player || mem_read32(cpu, at + 4u) != 0u)
        return false;
    int things = 0;
    for (u32 k = 0; k < HELD_SLOTS; k++) {
        const u32 thing = mem_read32(cpu, at + 8u + k * 8u), flags = mem_read32(cpu, at + 12u + k * 8u);
        if ((flags & 0xFFFFu) != 0u || flags > 0x00FF0000u)
            return false;
        if (thing != 0u && (thing < 0x80000000u || thing - 0x80000000u >= cpu->ram_size))
            return false;
        things += thing != 0u;
    }
    return things > 0;
}

static void held_find(CPUState* cpu, u32 player) {
    g_held_player = player;
    g_held_list = 0u;
    g_held_overridden = false;
    const u32 lo = player > 0x80000000u + HELD_SEARCH ? player - HELD_SEARCH : 0x80000000u;
    const u32 hi = player - 0x80000000u + HELD_SEARCH < cpu->ram_size ? player + HELD_SEARCH
                                                                       : 0x80000000u + cpu->ram_size - 64u;
    for (u32 at = lo & ~3u; at < hi; at += 4u)
        if (held_list_at(cpu, at, player)) {
            g_held_list = at;
            for (u32 k = 0; k < HELD_SLOTS; k++) {
                const u32 flags = mem_read32(cpu, at + 12u + k * 8u);
                g_held_shown[k] = flags & HELD_DRAWN ? flags : HELD_SHOWN;
            }
            if (g_log)
                fprintf(stderr, "[camera] what the player holds: list at 0x%08X\n", at);
            return;
        }
}

static void held_list_update(CPUState* cpu) {
    const u32 player = g_player_offset != 0 ? mem_read32(cpu, cpu->gpr[13] + (u32)(s32)g_player_offset) : 0u;
    if (player != g_held_player || (g_held_list != 0u && !held_list_at(cpu, g_held_list, player))) {
        if (player >= 0x80000000u && player - 0x80000000u < cpu->ram_size)
            held_find(cpu, player);
        else
            g_held_player = player, g_held_list = 0u, g_held_overridden = false;
    }
}

/* For the frame's draws: the flags as the camera has the player drawn; the
 * game's own go back once the frame is drawn (camera_held_end). */
void camera_held_begin(CPUState* cpu) {
    if (g_held_overridden || !g_view_active || g_menu_open)
        return;
    const int drawn = camera_player_drawn();
    held_list_update(cpu);
    if (g_held_list == 0u || drawn < 0)
        return;
    for (u32 k = 0; k < HELD_SLOTS; k++) {
        const u32 at = g_held_list + 12u + k * 8u, own = mem_read32(cpu, at);
        g_held_own[k] = own;
        if (mem_read32(cpu, at - 4u) == 0u)
            continue;  // empty
        // Shown, a thing keeps the game's own flags (a muzzle flash it shows
        // only as the gun fires), but one the game hides with the player (a
        // text box) and showed just before comes back as it was.
        if (!g_examining) {
            g_held_was_drawn[k] = (own & HELD_DRAWN) != 0u;
            if (g_held_was_drawn[k])
                g_held_shown[k] = own;
        }
        const u32 want = !drawn ? 0u : g_examining && g_held_was_drawn[k] ? g_held_shown[k] : own;
        g_held_written[k] = want;
        if (want != own)
            mem_write32(cpu, at, want);
    }
    g_held_overridden = true;
}

/* Hidden by the camera, a thing gets back its own flags, on the step the
 * frame's draw would have taken them (it took the camera's instead):
 * appearing (HELD_DRAWN alone) to shown, going (HELD_GOING alone) to gone.
 * Shown by the camera where the game hides it (a text box), a thing is left
 * as the draw left it, shown: ending the text the game brings it back from
 * whatever it finds, and given back its hidden own the draw would have
 * missed the start of that, leaving it hidden. */
#define HELD_GOING 0x00040000u
static u32 held_step(u32 own) {
    const u32 state = own & (HELD_DRAWN | HELD_GOING);
    return state == HELD_DRAWN ? own | HELD_GOING : state == HELD_GOING ? own & ~HELD_GOING : own;
}

void camera_held_end(CPUState* cpu) {
    if (!g_held_overridden || g_held_list == 0u)
        return;
    g_held_overridden = false;
    for (u32 k = 0; k < HELD_SLOTS; k++) {
        const u32 at = g_held_list + 12u + k * 8u;
        if (mem_read32(cpu, at) == g_held_written[k] && g_held_written[k] != g_held_own[k] &&
            (g_held_written[k] & HELD_DRAWN) == 0u)
            mem_write32(cpu, at, held_step(g_held_own[k]));
    }
}


/* Called at C_MTXLookAt from the game's camera (hle.c checks the return
 * address): r4 = eye, r5 = up, r6 = target; r1 = the camera function's stack
 * frame, whose bytes 40..63 it never uses (the eye and target written there
 * are read once, by LookAt). The world's up is the camera's up vector (+Z in
 * this game). */
void camera_look_at(CPUState* cpu) {
    g_cpu = cpu;
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
    // The Tome draws the inventory's things with this camera too: while it is
    // open the game has its own view (and they theirs), and the camera's own
    // state waits as it was for the Tome to close.
    if (g_menu_open) {
        g_view_active = false;
        return;
    }
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
        g_hand_progress = g_hand = g_fp_hand_fov = 0.0f;
        return;
    }
    const bool still = g_aiming;
    g_head_eye = add(player, scale(k, g_head_valid ? clampf(g_eye_height, FP_MIN_EYE, FP_MAX_EYE) : FP_HEIGHT));
    Vec3 new_eye, new_target;
    if (mode == CAMERA_FIRST_PERSON) {
        Vec3 focus = target;
        game_goal(cpu, &focus);
        first_person(eye, target, focus, player, k, step, still, &new_eye, &new_target);
    } else {
        // What the player looks at: where the game's camera is heading,
        // known from the moment they press B (else where it looks now).
        Vec3 focus = target;
        game_goal(cpu, &focus);
        third_person(cpu, eye, focus, player, k, step, still, &new_eye, &new_target);
    }
    // First person (ED_CAM_FACE=1: every mode, tests): no turning round.
    static int face_all = -1;
    if (face_all < 0)
        face_all = getenv("ED_CAM_FACE") != NULL && atoi(getenv("ED_CAM_FACE")) != 0;
    if (step && !still && g_hand_progress <= 0.0f && (mode == CAMERA_FIRST_PERSON || face_all))
        face_stick(cpu, sub(new_target, new_eye), k);
    // Aiming in first person: facing the view, as they raise the gun.
    if (step && still && g_hand_progress <= 0.0f && mode == CAMERA_FIRST_PERSON && g_aim_frames <= FP_AIM_TURN)
        player_set_facing(cpu, normalize(flatten(sub(new_target, new_eye), k)));
    // Looking closely at something, the view eases over to the game's own
    // camera (where it stands, what it looks at, its field of view: its framing
    // of the thing) and back when that ends, on the sweep's eased timing.
    if (step) {
        // In first person only the player's own look (B) hands over: the
        // game's zoom as they walk up to a thing is left to the prompt.
        const bool known = g_looking_known >= 0;
        const bool zoom = g_zoomed && !g_zooming_out && (mode != CAMERA_FIRST_PERSON || !known);
        const bool handing = g_examining || (known && g_looking_known != 0) || zoom;
        const float time = mode == CAMERA_FIRST_PERSON ? FP_HAND_TIME : SWEEP_TIME;
        g_hand_progress = clampf(g_hand_progress + (handing ? 1.0f : -1.0f) / (60.0f * time), 0.0f, 1.0f);
        g_hand = mode == CAMERA_FIRST_PERSON
                     ? smoother((g_hand_progress - FP_HAND_MOVE) / (1.0f - FP_HAND_MOVE))
                     : sweep_ease(g_hand_progress);
        g_handing = handing;
    }
    if (g_handing || g_hand_progress <= 0.0f) {
        g_held_eye = eye;
        g_held_target = target;
        g_held_fov = read_float(cpu, cpu->gpr[4] + 48u);
    }
    if (mode == CAMERA_FIRST_PERSON && g_hand_progress > 0.0f) {
        // The eye on its way (g_hand also eases the field of view), the
        // view turned from its own way to the thing seen from there.
        const float aim = smoother(g_hand_progress / FP_HAND_AIM);
        const Vec3 own = sub(new_target, new_eye);
        const Vec3 from = add(new_eye, scale(sub(g_held_eye, new_eye), g_hand));
        const Vec3 thing = sub(g_held_target, from);
        // A thing close to first person's eye (beside it, behind it) is no
        // direction to turn by: the view turns to the game's own instead,
        // chosen once a move.
        if (!g_fp_hand_decided) {
            g_fp_hand_decided = true;
            g_fp_hand_near = length(sub(g_held_target, g_fp_eye)) < FP_HAND_NEAR;
        }
        const Vec3 goal = g_fp_hand_near ? sub(g_held_target, g_held_eye) : thing;
        const Vec3 dir = turn_towards(normalize(own), normalize(goal), aim);
        new_eye = from;
        new_target = add(from, scale(dir, length(own) + (length(thing) - length(own)) * aim));
        // The thing's size on screen, 1 / (distance x tan(half the field
        // of view)), eased from first person's to the game's (evenly in
        // proportion), and the field of view that gives it from here.
        const float d0 = fmaxf(length(sub(g_held_target, g_fp_eye)), 1.0f);
        const float d1 = fmaxf(length(sub(g_held_target, g_held_eye)), 1.0f);
        const float d = fmaxf(length(thing), 1.0f);
        const float half = PI / 360.0f;
        const float game_fov = g_held_fov > 0.0f ? g_held_fov : FP_FOV;
        const float s0 = 1.0f / (d0 * tanf(FP_FOV * half)), s1 = 1.0f / (d1 * tanf(game_fov * half));
        const float size = s0 * powf(s1 / s0, g_hand);
        // Never wider or narrower than either end (close to the eye the size
        // runs away).
        g_fp_hand_fov = g_fp_hand_near ? FP_FOV + (game_fov - FP_FOV) * g_hand
                                       : clampf(atanf(1.0f / (size * d)) / half, fminf(FP_FOV, game_fov),
                                                fmaxf(FP_FOV, game_fov));
    } else {
        g_fp_hand_fov = 0.0f;
        g_fp_hand_decided = false;
        new_eye = add(new_eye, scale(sub(g_held_eye, new_eye), g_hand));
        new_target = add(new_target, scale(sub(g_held_target, new_target), g_hand));
    }
    if (step) {
        g_last_eye = eye;
        g_last_target = target;
        g_have_last = true;
        if (!still)
            g_turn = level_angle(sub(target, eye), sub(new_target, new_eye), k);
    }
    g_view_active = true;
    camera_held_begin(cpu);
    g_view_eye = new_eye;
    g_view_target = new_target;
    if (g_log && step && getenv("ED_CAM_LOG")[0] == '4')
        fprintf(stderr,
                "[camera] view %llu eye %.2f %.2f %.2f target %.2f %.2f %.2f hand %.3f game %.2f %.2f %.2f %.2f %.2f "
                "%.2f fov %.1f look %d hidden %d ours %.2f\n",
                frame, new_eye.x, new_eye.y, new_eye.z, new_target.x, new_target.y, new_target.z, g_hand, eye.x,
                eye.y, eye.z, target.x, target.y, target.z, read_float(cpu, cpu->gpr[4] + 48u), g_looking_known,
                g_examining, camera_field_of_view(read_float(cpu, cpu->gpr[4] + 48u)));
    const u32 sp = cpu->gpr[1];
    write_vec(cpu, sp + 40u, new_eye);
    write_vec(cpu, sp + 52u, new_target);
    cpu->gpr[4] = sp + 40u;
    cpu->gpr[6] = sp + 52u;
}
