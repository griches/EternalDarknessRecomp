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
 * both; while aiming they hold still. The game moves the player relative to
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

// From the controls (the game thread's pad read).
static float g_stick_x, g_stick_y;
static bool g_aiming, g_play;
static float g_turn;  // camera_stick_turn

void camera_set_stick(float x, float y, bool aiming, bool play) {
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
#define TP_PITCH 0.30f

static int g_last_mode = -1;
// Free look: the turn from the game camera's view (yaw about up, pitch).
static float g_yaw, g_pitch;
static Vec3 g_last_eye, g_last_target;
static bool g_have_last;
// Third person: the level direction from the player to the eye, the eye's
// height (radians above level), and where the player was.
static Vec3 g_back;
static float g_rise;
static Vec3 g_last_player;
static bool g_have_player;

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

/* Third person: behind and above the player, trailing after them. Starts
 * from the side the game's camera is on. */
static void third_person(Vec3 eye, Vec3 player, Vec3 k, bool step, bool still, Vec3* new_eye,
                         Vec3* new_target) {
    const Vec3 pivot = add(player, scale(k, TP_HEIGHT));
    if (step) {
        const bool jumped = !g_have_player || length(sub(player, g_last_player)) > 300.0f;
        if (jumped) {
            g_back = normalize(flatten(sub(eye, pivot), k));
            if (length(g_back) == 0.0f)
                g_back = normalize(flatten(v3(1.0f, 0.0f, 0.0f), k));
            g_rise = TP_PITCH;
        } else if (!still) {
            // Trail: the eye stays where it was as the player walks off,
            // drawn back out to its distance along the line to them.
            const Vec3 old_eye = add(add(g_last_player, scale(k, TP_HEIGHT)),
                                     scale(g_back, TP_DISTANCE * cosf(g_rise)));
            const Vec3 back = normalize(flatten(sub(old_eye, pivot), k));
            if (length(back) > 0.0f)
                g_back = back;
            const float dt = 1.0f / 60.0f;
            g_back = normalize(rotate(g_back, k, -g_stick_x * YAW_RATE * dt));
            g_rise = clampf(g_rise - g_stick_y * PITCH_RATE * dt, -0.35f, 1.2f);
        }
        g_last_player = player;
        g_have_player = true;
    }
    *new_eye = add(pivot, add(scale(g_back, TP_DISTANCE * cosf(g_rise)), scale(k, TP_DISTANCE * sinf(g_rise))));
    *new_target = pivot;
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
    if (g_log && step && frame % 120u == 0u)
        fprintf(stderr, "[camera] frame %llu eye %.1f %.1f %.1f target %.1f %.1f %.1f up %.2f %.2f %.2f player %s%.1f %.1f %.1f\n",
                frame, eye.x, eye.y, eye.z, target.x, target.y, target.z, up.x, up.y, up.z,
                have_player ? "" : "(none) ", player.x, player.y, player.z);
    int mode = g_play ? camera_mode() : CAMERA_ORIGINAL;
    if (mode == CAMERA_THIRD_PERSON && !have_player)
        mode = CAMERA_FREE_LOOK;
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
        return;
    }
    const bool still = g_aiming;
    Vec3 new_eye, new_target;
    if (mode == CAMERA_FREE_LOOK)
        free_look(eye, target, k, step, still, &new_eye, &new_target);
    else
        third_person(eye, player, k, step, still, &new_eye, &new_target);
    if (step) {
        g_last_eye = eye;
        g_last_target = target;
        g_have_last = true;
        if (!still)
            g_turn = level_angle(sub(target, eye), sub(new_target, new_eye), k);
    }
    const u32 sp = cpu->gpr[1];
    write_vec(cpu, sp + 40u, new_eye);
    write_vec(cpu, sp + 52u, new_target);
    cpu->gpr[4] = sp + 40u;
    cpu->gpr[6] = sp + 52u;
}
