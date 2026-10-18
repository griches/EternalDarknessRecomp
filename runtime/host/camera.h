#pragma once
// Camera options (camera.c): the game's own fixed cameras, a free look from
// where the game's camera stands, or a third-person camera that follows Alex.
// The right stick turns it; while aiming it holds still.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CAMERA_ORIGINAL, CAMERA_FREE_LOOK, CAMERA_THIRD_PERSON, CAMERA_MODE_COUNT };

const char* camera_mode_name(int mode);
// The mode in use: settings.ini's camera, or ED_CAM=0/1/2 for a test run.
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
// something (its camera at their head, looking at the thing).
void camera_set_examining(bool examining);
// Third person is in use: the player stays drawn even when the game hides
// them to look closely at something.
bool camera_shows_player(void);
// The right stick belongs to the camera (a mode is on, the game is in play).
bool camera_takes_stick(void);
// The camera's turn from the game's own, for the left stick: the angle (radians,
// counter-clockwise seen from above) to turn the stick by so that pushing up
// walks away from the camera. 0 while aiming or with the game's camera.
float camera_stick_turn(void);

#ifdef __cplusplus
}
#endif
