#include "input.h"
#include "emu.h"
#include "view.h"

#include <math.h>
#include <string.h>

#define MAX_FINGERS 10
#define MAX_PADS 4
#define NS_MS 1000000ull

#define HOLD_NS (600ull * NS_MS)   // M long press
#define TAP_MAX_NS (320ull * NS_MS)
#define TWO_TAP_NS (350ull * NS_MS)
#define TAP_SLOP 0.055f            // fraction of the short screen side
#define STICK_RADIUS 0.15f
#define STICK_ON 0.38f             // deflection that starts a direction
#define STICK_KEEP 0.26f           // ...and keeps it (hysteresis)
#define STICK_SWITCH_DEG 12.0f     // extra angle needed to change direction (no jitter on diagonals)
#define PAD_DEADZONE 0.35f

#define PULSE_ON 3                 // frames a synthetic press is held
#define PULSE_OFF 3                // frames released before the next one
#define MAX_PULSES 16

typedef struct {
	bool used;
	SDL_FingerID id;
	SDL_TouchID touch;
	uint32_t bits;      // rim button
	bool gesture;       // free-area finger
	float x0, y0;
	uint64_t downNs;
	bool moved;         // left the tap slop -> stick
	bool consumed;      // part of a multi-finger gesture, no tap on release
} Finger;

static struct {
	int w, h;
	InputHitFn hit;
	bool iso;

	Finger fingers[MAX_FINGERS];
	TouchStick stick;
	SDL_FingerID stickId;
	bool touchSeen;

	uint64_t modeDownNs;
	bool modeHeld, modeHoldFired;
	uint32_t actions;

	SDL_Gamepad* pads[MAX_PADS];

	// Synthetic presses (taps, crown detents, back button), played one after another.
	uint16_t pulse[MAX_PULSES];
	int pulseHead, pulseCount, pulsePhase;
	uint16_t pulseNow;

	uint32_t tapLatch;  // rim buttons released before the game could see them
	int tapFrames;
	int stickMinFrames; // a new stick direction is held at least this long (flicks = 1 step)
	uint16_t stickLatched;
} S;

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

void input_init(void) {
	memset(&S, 0, sizeof(S));
}

void input_shutdown(void) {
	for (int i = 0; i < MAX_PADS; ++i) {
		if (S.pads[i]) {
			SDL_CloseGamepad(S.pads[i]);
		}
	}
}

void input_set_hit_fn(InputHitFn fn) { S.hit = fn; }

void input_set_screen(int w, int h) {
	S.w = w;
	S.h = h;
	S.stick.radius = STICK_RADIUS * (float) (w < h ? w : h);
}

bool input_touch_seen(void) { return S.touchSeen; }
const TouchStick* input_stick(void) { return &S.stick; }
void input_set_iso(bool on) { S.iso = on; }
bool input_iso(void) { return S.iso; }

uint32_t input_take_actions(void) {
	uint32_t a = S.actions;
	S.actions = 0;
	return a;
}

static void pulse_push(uint16_t key) {
	if (S.pulseCount >= MAX_PULSES) {
		return;
	}
	S.pulse[(S.pulseHead + S.pulseCount) % MAX_PULSES] = key;
	++S.pulseCount;
}

void input_crown(int detents) {
	uint16_t k = detents > 0 ? KEY_DOWN : KEY_UP;
	int n = detents > 0 ? detents : -detents;
	// Never let a fast spin queue up a long backlog of presses.
	for (int i = 0; i < n && S.pulseCount < 4; ++i) {
		pulse_push(k);
	}
}

// ------------------------------------------------------------------ touch

static float slop_px(void) {
	return TAP_SLOP * (float) (S.w < S.h ? S.w : S.h);
}

static Finger* finger_find(SDL_TouchID t, SDL_FingerID f) {
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (S.fingers[i].used && S.fingers[i].id == f && S.fingers[i].touch == t) {
			return &S.fingers[i];
		}
	}
	return NULL;
}

static int gesture_fingers(Finger** out, int max) {
	int n = 0;
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (S.fingers[i].used && S.fingers[i].gesture) {
			if (n < max) {
				out[n] = &S.fingers[i];
			}
			++n;
		}
	}
	return n;
}

static void finger_down(SDL_TouchID t, SDL_FingerID f, float x, float y) {
	Finger* fg = NULL;
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (!S.fingers[i].used) {
			fg = &S.fingers[i];
			break;
		}
	}
	if (!fg) {
		return;
	}
	memset(fg, 0, sizeof(*fg));
	fg->used = true;
	fg->id = f;
	fg->touch = t;
	fg->x0 = x;
	fg->y0 = y;
	fg->downNs = SDL_GetTicksNS();

	uint32_t bits = S.hit ? S.hit(x, y) : 0;
	if (bits) {
		fg->bits = bits;
		if (bits & VIEW_MODE) {
			S.modeHeld = true;
			S.modeHoldFired = false;
			S.modeDownNs = fg->downNs;
		}
		return;
	}
	fg->gesture = true;
}

static void stick_start(Finger* fg, float x, float y) {
	fg->moved = true;
	S.stickId = fg->id;
	S.stick.active = true;
	S.stick.ax = fg->x0;
	S.stick.ay = fg->y0;
	S.stick.x = x;
	S.stick.y = y;
	S.stick.dir = 0;
}

static void finger_motion(Finger* fg, float x, float y) {
	if (!fg->gesture || fg->consumed) {
		return;
	}
	if (!fg->moved) {
		float dx = x - fg->x0, dy = y - fg->y0;
		if (dx * dx + dy * dy < slop_px() * slop_px() || S.stick.active) {
			return;
		}
		stick_start(fg, x, y);
	}
	if (!S.stick.active || S.stickId != fg->id) {
		return;
	}
	S.stick.x = x;
	S.stick.y = y;
	float dx = x - S.stick.ax, dy = y - S.stick.ay;
	float d = sqrtf(dx * dx + dy * dy);
	if (d > S.stick.radius) {
		// Trail: drag the anchor behind the thumb so direction changes are instant.
		S.stick.ax = x - dx / d * S.stick.radius;
		S.stick.ay = y - dy / d * S.stick.radius;
	}
}

static void finger_up(Finger* fg, bool canceled) {
	uint64_t now = SDL_GetTicksNS();
	if (fg->gesture) {
		if (S.stick.active && S.stickId == fg->id && fg->moved) {
			S.stick.active = false;
			S.stick.dir = 0;
		}
		Finger* g[3];
		int n = gesture_fingers(g, 3);
		if (!canceled && !fg->consumed && !fg->moved && now - fg->downNs <= TAP_MAX_NS) {
			if (n == 1) {
				pulse_push(KEY_A);  // single quick tap = confirm
			}
		}
		if (!canceled && !fg->moved && n >= 2 && !fg->consumed) {
			// Second finger tapped while another is down.
			Finger* other = g[0] == fg ? g[1] : g[0];
			if (other->moved) {
				pulse_push(KEY_A);  // dragging the cursor + tap = confirm
			} else if (!other->consumed && now - other->downNs <= TWO_TAP_NS + TAP_MAX_NS) {
				pulse_push(KEY_B);  // two-finger tap = cancel
				other->consumed = true;
			}
		}
	}
	if (fg->bits & 0x3FF) {
		// Keep quick rim taps visible to the game for a few frames.
		S.tapLatch |= fg->bits & 0x3FF;
		S.tapFrames = PULSE_ON;
	}
	if ((fg->bits & VIEW_FAST) && !canceled) {
		S.actions |= ACT_FAST;
	}
	if ((fg->bits & VIEW_MODE) && S.modeHeld) {
		S.modeHeld = false;
		if (!S.modeHoldFired && !canceled) {
			S.actions |= ACT_SCREEN;
		}
	}
	fg->used = false;
}

uint32_t input_touch_held(void) {
	uint32_t bits = 0;
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (S.fingers[i].used) {
			bits |= S.fingers[i].bits;
		}
	}
	return bits;
}

uint16_t input_gesture_keys(void) {
	return S.pulseNow | S.stick.dir;
}

// ------------------------------------------------------------------ events

void input_handle_event(const SDL_Event* e) {
	switch (e->type) {
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED: {
		float x = e->tfinger.x * (float) S.w;
		float y = e->tfinger.y * (float) S.h;
		if (e->tfinger.touchID != SDL_MOUSE_TOUCHID) {
			S.touchSeen = true;
		}
		if (e->type == SDL_EVENT_FINGER_DOWN) {
			finger_down(e->tfinger.touchID, e->tfinger.fingerID, x, y);
		} else {
			Finger* fg = finger_find(e->tfinger.touchID, e->tfinger.fingerID);
			if (!fg) {
				break;
			}
			if (e->type == SDL_EVENT_FINGER_MOTION) {
				finger_motion(fg, x, y);
			} else {
				finger_up(fg, e->type == SDL_EVENT_FINGER_CANCELED);
			}
		}
		break;
	}
	case SDL_EVENT_GAMEPAD_ADDED:
		for (int i = 0; i < MAX_PADS; ++i) {
			if (!S.pads[i]) {
				S.pads[i] = SDL_OpenGamepad(e->gdevice.which);
				if (S.pads[i]) {
					char buf[96];
					SDL_snprintf(buf, sizeof(buf), "Controle: %s", SDL_GetGamepadName(S.pads[i]));
					view_toast(buf);
				}
				break;
			}
		}
		break;
	case SDL_EVENT_GAMEPAD_REMOVED:
		for (int i = 0; i < MAX_PADS; ++i) {
			if (S.pads[i] && SDL_GetGamepadID(S.pads[i]) == e->gdevice.which) {
				SDL_CloseGamepad(S.pads[i]);
				S.pads[i] = NULL;
			}
		}
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		if (e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
			S.actions |= ACT_FAST;
		}
		break;
	case SDL_EVENT_KEY_DOWN:
		// Watch/phone back button (trapped by SDL) -> B, i.e. cancel / close menu.
		if (e->key.scancode == SDL_SCANCODE_AC_BACK && !e->key.repeat) {
			pulse_push(KEY_B);
		}
		break;
	default:
		break;
	}
}

// ------------------------------------------------------------------ per frame

// Analog vector -> one D-pad direction with hysteresis, so the cursor never jitters
// between two directions while the thumb rests on a diagonal.
static uint16_t quantize(float x, float y, uint16_t prev) {
	float m = sqrtf(x * x + y * y);
	if (m < (prev ? STICK_KEEP : STICK_ON)) {
		return 0;
	}
	float deg = atan2f(y, x) * 57.29578f;  // 0 = right, 90 = down
	static const struct { float c; uint16_t bit; } dirs[] = {
		{ 0, KEY_RIGHT }, { 90, KEY_DOWN }, { 180, KEY_LEFT }, { -90, KEY_UP }, { -180, KEY_LEFT },
	};
	uint16_t best = 0;
	float bestD = 1e9f;
	for (int i = 0; i < 5; ++i) {
		float d = fabsf(deg - dirs[i].c);
		if (dirs[i].bit == prev) {
			d -= STICK_SWITCH_DEG;  // current direction wins ties near the diagonal
		}
		if (d < bestD) {
			bestD = d;
			best = dirs[i].bit;
		}
	}
	return best;
}

uint16_t input_frame(void) {
	uint16_t keys = 0;

	if (S.modeHeld && !S.modeHoldFired && SDL_GetTicksNS() - S.modeDownNs > HOLD_NS) {
		S.modeHoldFired = true;
		S.actions |= ACT_ISO;
	}

	// Keyboard
	const bool* ks = SDL_GetKeyboardState(NULL);
	if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]) keys |= KEY_UP;
	if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) keys |= KEY_DOWN;
	if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) keys |= KEY_LEFT;
	if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) keys |= KEY_RIGHT;
	if (ks[SDL_SCANCODE_Z] || ks[SDL_SCANCODE_K] || ks[SDL_SCANCODE_SPACE]) keys |= KEY_A;
	if (ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_J]) keys |= KEY_B;
	if (ks[SDL_SCANCODE_RETURN]) keys |= KEY_START;
	if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) keys |= KEY_SELECT;
	if (ks[SDL_SCANCODE_Q]) keys |= KEY_L;
	if (ks[SDL_SCANCODE_E]) keys |= KEY_R;

	// Rim buttons
	keys |= (uint16_t) (input_touch_held() & 0x3FF);
	if (S.tapFrames > 0) {
		keys |= (uint16_t) S.tapLatch;
		if (--S.tapFrames == 0) {
			S.tapLatch = 0;
		}
	}

	// Synthetic presses: PULSE_ON frames pressed, PULSE_OFF released, next.
	S.pulseNow = 0;
	if (S.pulseCount > 0) {
		if (S.pulsePhase < PULSE_ON) {
			S.pulseNow = S.pulse[S.pulseHead];
			keys |= S.pulseNow;
		}
		if (++S.pulsePhase >= PULSE_ON + PULSE_OFF) {
			S.pulsePhase = 0;
			S.pulseHead = (S.pulseHead + 1) % MAX_PULSES;
			--S.pulseCount;
		}
	}

	// Gamepads
	for (int i = 0; i < MAX_PADS; ++i) {
		SDL_Gamepad* p = S.pads[i];
		if (!p) {
			continue;
		}
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH)) keys |= KEY_A;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST)) keys |= KEY_B;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) keys |= KEY_START;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK)) keys |= KEY_SELECT;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) keys |= KEY_L;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) keys |= KEY_R;
		if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) keys |= KEY_L;
		if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) keys |= KEY_R;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP)) keys |= KEY_UP;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) keys |= KEY_DOWN;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) keys |= KEY_LEFT;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) keys |= KEY_RIGHT;
		float sx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
		float sy = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
		if (fabsf(sx) > PAD_DEADZONE || fabsf(sy) > PAD_DEADZONE) {
			keys |= fabsf(sx) > fabsf(sy) ? (sx < 0 ? KEY_LEFT : KEY_RIGHT) : (sy < 0 ? KEY_UP : KEY_DOWN);
		}
	}

	// Finger trail-stick -> one D-pad direction
	if (S.stick.active) {
		float dx = (S.stick.x - S.stick.ax) / S.stick.radius;
		float dy = (S.stick.y - S.stick.ay) / S.stick.radius;
		if (S.iso) {
			// Screen up-right -> UP, up-left -> LEFT, down-left -> DOWN, down-right -> RIGHT.
			const float k = 0.70710678f;
			float rx = (dx + dy) * k, ry = (dy - dx) * k;
			dx = rx;
			dy = ry;
		}
		uint16_t d = quantize(clampf(dx, -1.5f, 1.5f), clampf(dy, -1.5f, 1.5f), S.stick.dir);
		if (d && d != S.stick.dir) {
			S.stickLatched = d;
			S.stickMinFrames = 2;  // short flicks still register as one cursor step
		}
		S.stick.dir = d;
	} else {
		S.stick.dir = 0;
	}
	if (S.stick.dir) {
		keys |= S.stick.dir;
	} else if (S.stickMinFrames > 0) {
		keys |= S.stickLatched;
	}
	if (S.stickMinFrames > 0) {
		--S.stickMinFrames;
	}

	// Never send opposing directions (keyboard + touch conflicts).
	if ((keys & (KEY_LEFT | KEY_RIGHT)) == (KEY_LEFT | KEY_RIGHT)) keys &= ~(KEY_LEFT | KEY_RIGHT);
	if ((keys & (KEY_UP | KEY_DOWN)) == (KEY_UP | KEY_DOWN)) keys &= ~(KEY_UP | KEY_DOWN);
	return keys;
}
