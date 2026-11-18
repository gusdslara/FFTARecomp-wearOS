// Unified input for a tactics game on a watch: touch gestures, rotary crown, gamepads, keyboard.
//
// Touch (anywhere outside the rim buttons):
//   drag            -> D-pad (trail stick, 4-way, hold = game's own key repeat)
//   quick tap       -> A (confirm)
//   two-finger tap  -> B (cancel)
//   tap a 2nd finger while dragging -> A (move the cursor and confirm without lifting)
// Rotary crown / bezel / mouse wheel -> D-pad up/down, one press per detent (menus, lists).
// Watch back button -> B.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

// Edge actions for the frontend (not GBA keys).
enum {
	ACT_SCREEN = 1 << 0,  // cycle screen preset (M tap)
	ACT_ISO = 1 << 1,     // toggle isometric drag mapping (M hold)
	ACT_FAST = 1 << 2,    // toggle fast-forward (>> tap)
};

// Hit-test callback: returns GBA key bits / VIEW_* bits for a pixel position.
typedef uint32_t (*InputHitFn)(float x, float y);

typedef struct {
	bool active;
	float ax, ay;  // anchor (px)
	float x, y;    // current finger (px)
	float radius;  // px
	uint16_t dir;  // D-pad bit currently produced (0 = none)
} TouchStick;

void input_init(void);
void input_shutdown(void);
void input_set_hit_fn(InputHitFn fn);
void input_set_screen(int w, int h);

void input_handle_event(const SDL_Event* e);
// Edge actions (ACT_*) since the last call.
uint32_t input_take_actions(void);

// Call once per emulated frame. Returns the GBA key mask.
uint16_t input_frame(void);

// Rotary crown: positive = down the list.
void input_crown(int detents);

// Held touch-overlay bits (GBA keys + VIEW_* bits) for drawing pressed state.
uint32_t input_touch_held(void);
// GBA keys pressed by gestures right now (tap pulses etc.), for feedback.
uint16_t input_gesture_keys(void);
const TouchStick* input_stick(void);
bool input_touch_seen(void);  // a real (non-mouse) touch happened

// Isometric mapping: rotate drags 45 degrees so screen diagonals follow the battle grid.
void input_set_iso(bool on);
bool input_iso(void);
