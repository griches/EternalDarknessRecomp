#pragma once
// Camera options (camera.c): the game's own fixed cameras, a third-person
// camera that follows Alex, or first person through her eyes. The right
// stick turns it; while aiming it holds still.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// settings.ini's numbers (1 was a free look, gone: it reads as the original).
enum { CAMERA_ORIGINAL = 0, CAMERA_THIRD_PERSON = 2, CAMERA_FIRST_PERSON = 3, CAMERA_MODE_COUNT = 4 };

const char* camera_mode_name(int mode);
// The mode in use: settings.ini's camera, or ED_CAM=0/2/3 for a test run.
int camera_mode(void);
// From the controls, once a pad read: the right stick (-1..1, up positive)
// while it turns the camera, whether the player is aiming (R held), and
// whether the game is in play (not a cutscene or the Tome).
void camera_set_stick(float x, float y, bool aiming, bool play);
// From hle.c: the player object's pointer, at r13 + offset (0: not found).
void camera_set_player_offset(int offset);
// From hle.c: the game's own line test against the room's walls, floors and
// ceilings, from start to end with a radius: true and the first point hit
// (world coordinates), or false when the line is clear (or the test failed).
struct CPUState;
typedef bool (*CameraCollide)(struct CPUState* cpu, const float start[3], const float end[3], float radius,
                              float hit[3]);
void camera_set_collide(CameraCollide fn);
// The eye and target this camera drew the last frame from, while it is the
// one in use (false with the game's own), for game code that measures from
// the camera (hle.c: the fade of things close to it).
bool camera_view(float eye[3], float target[3]);
// From hle.c, each frame: the game has hidden the player to look closely at
// something (its camera at their head), and whether the player is looking
// closely at something at all (B until they return; -1: not known, the
// game's zoom tells instead).
void camera_set_examining(bool hidden, int looking);
// Whether the player is drawn: 1 always (third person: even when the game
// hides them to look closely at something), 0 never (first person: the eye
// is in their head), -1 as the game has it.
int camera_player_drawn(void);
// From hle.c, as the game's camera makes its projection: the field of view
// (degrees) to use instead of the game's.
float camera_field_of_view(float game_fov);
// ... and the near clipping distance: in first person it is moved out past
// the player's hair (drawn round the eye), so their body below still shows.
float camera_near_plane(float game_near, unsigned long long frame);  // frame: timebase / 675000
// From hle.c: the game camera's eye and target controllers (each eases
// toward the point its +112 points at).
void camera_set_controllers(unsigned eye, unsigned target);
// From hle.c: the game works out each bone of a character relative to its
// parent, one at a time (fn(character, bone, out matrix)); camera.c follows
// the player's to find their head (first person).
void camera_bone_matrix(struct CPUState* cpu, unsigned character, unsigned bone, unsigned out);

// The third-person camera's own state (where it is round the player, how
// high and far), for input recordings: a replay starts where it was.
enum { CAMERA_STATE_SIZE = 8 };
void camera_get_state(float out[CAMERA_STATE_SIZE]);
void camera_set_state(const float in[CAMERA_STATE_SIZE]);
// The right stick belongs to the camera (a mode is on, the game is in play).
bool camera_takes_stick(void);
// The camera's turn from the game's own, for the left stick: the angle (radians,
// counter-clockwise seen from above) to turn the stick by so that pushing up
// walks away from the camera. 0 while aiming or with the game's camera.
float camera_stick_turn(void);
// The left stick as the player pushes it (-1..1, up +y), before the turn: in
// first person the player faces where it points at once, without the game's
// turn round.
void camera_set_move(float x, float y);
// PADRead: the buttons the game is about to get (the action button turns the
// player to face the view in first person). True while the game should get
// no main stick (it would turn them back).
bool camera_pad_buttons(struct CPUState* cpu, unsigned buttons);
// Whether the game's Tome (its own menu) is open: what the player holds is then
// left as the game has it (the Tome shows the inventory's things itself).
void camera_set_menu(bool open);
// What the player holds set as the camera has them drawn for the frame's
// draws (from the camera), and the game's own put back once they are done
// (hle.c, GXSetDrawDone) or before a state is saved: the game's logic and a
// saved state never see the camera's.
struct CPUState;
void camera_held_begin(struct CPUState* cpu);
void camera_held_end(struct CPUState* cpu);
// Around a character's bones being worked out and its mesh skinned (hle.c):
// aiming in first person, the player's bones but their arms' scaled to
// nothing (only their arms drawn), and given back after.
void camera_skin_begin(struct CPUState* cpu, unsigned character);
void camera_skin_end(struct CPUState* cpu, unsigned character);

#ifdef __cplusplus
}
#endif
