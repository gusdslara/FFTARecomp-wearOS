#include "view.h"
#include "emu.h"
#include "input.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- tokens
// Neutral translucent white + one accent (parchment gold, matches the game's menus).
static const SDL_FColor C_BTN = { 1.00f, 1.00f, 1.00f, 0.16f };
static const SDL_FColor C_BTN_EDGE = { 1.00f, 1.00f, 1.00f, 0.38f };
static const SDL_FColor C_BTN_ON = { 0.93f, 0.76f, 0.38f, 0.75f };
static const SDL_FColor C_ARC = { 1.00f, 1.00f, 1.00f, 0.11f };
static const SDL_FColor C_LABEL = { 1.00f, 1.00f, 1.00f, 0.90f };
static const SDL_FColor C_STICK = { 1.00f, 1.00f, 1.00f, 0.24f };
static const SDL_FColor C_KNOB = { 0.93f, 0.76f, 0.38f, 0.65f };
static const SDL_FColor C_TOAST_BG = { 0.05f, 0.05f, 0.07f, 0.82f };
static const SDL_FColor C_MASK = { 0, 0, 0, 1 };
#define TOAST_NS (1800ull * 1000000ull)
#define IDLE_FADE_NS (4000ull * 1000000ull)

#define PI_F 3.14159265f
#define DEG(d) ((d) * PI_F / 180.0f)

// Round layout: the whole GBA frame, undistorted, is the largest 3:2 rectangle that fits the
// glass (IMG_RHO): nothing cropped, nothing bent. The buttons fill the free caps above and
// below it and the thin crescents at its sides; their inner edge follows the picture border,
// so no button ever covers a game pixel and nothing reaches past the glass.
#define IMG_RHO 0.985f
#define BTN_GAP 0.022f  // space between the picture and a button (fraction of R)
#define ARC_OUT 0.985f

// Optional undistorted zoom (M button) for small sprites; the edges then go under the buttons.
static const float PRESET_ZOOM[SCREEN_COUNT] = { 1.0f, 1.15f, 1.30f };

typedef struct {
	float cx, cy, r;   // circle buttons (rectangular screens)
	float a0, a1;      // arc buttons (round screens), degrees, y-down
	bool arc;
	uint32_t bits;
	const char* label;
} Btn;

static struct {
	int w, h;
	bool round;
	float cx, cy, R;
	Btn btn[10];
	int nbtn;
	ScreenPreset screen;
	float zoomCur;  // animated zoom, towards PRESET_ZOOM[screen]
	bool overlay, mask, smooth, fast;
	char toast[96];
	uint64_t toastNs;
	char debug[128];

	SDL_Texture* up;  // 3x nearest-upscaled frame, sampled linearly (crisp, no shimmer)
	uint64_t lastTouchNs;
} V = { .screen = SCREEN_FULL, .zoomCur = 0.0f, .overlay = true };

void view_set_overlay(bool on) { V.overlay = on; }
bool view_overlay(void) { return V.overlay; }
void view_set_mask(bool on) { V.mask = on; }
void view_set_smooth(bool on) { V.smooth = on; }
bool view_smooth(void) { return V.smooth; }
void view_set_fast(bool on) { V.fast = on; }

void view_set_screen(ScreenPreset p) {
	V.screen = (ScreenPreset) SDL_clamp((int) p, 0, SCREEN_COUNT - 1);
}
ScreenPreset view_screen(void) { return V.screen; }

const char* view_screen_name(ScreenPreset p) {
	switch (p) {
	case SCREEN_FULL: return "Tela inteira";
	case SCREEN_ZOOM: return "Zoom";
	case SCREEN_ZOOM_MORE: return "Zoom forte";
	default: return "?";
	}
}

void view_toast(const char* msg) {
	SDL_strlcpy(V.toast, msg, sizeof(V.toast));
	V.toastNs = SDL_GetTicksNS();
	SDL_Log("%s", msg);
}

void view_set_debug(const char* t) {
	if (t) {
		SDL_strlcpy(V.debug, t, sizeof(V.debug));
	} else {
		V.debug[0] = 0;
	}
}

static void add_btn(float cx, float cy, float r, uint32_t bits, const char* label) {
	V.btn[V.nbtn++] = (Btn) { .cx = cx, .cy = cy, .r = r, .bits = bits, .label = label };
}

static void add_arc(float a0, float a1, uint32_t bits, const char* label) {
	V.btn[V.nbtn++] = (Btn) { .a0 = a0, .a1 = a1, .arc = true, .bits = bits, .label = label };
}

void view_layout(int w, int h, bool round) {
	V.w = w;
	V.h = h;
	V.round = round;
	V.cx = w * 0.5f;
	V.cy = h * 0.5f;
	V.R = (w < h ? w : h) * 0.5f;
	V.nbtn = 0;
	if (round) {
		// Degrees, 0 = right, 90 = bottom. The picture corners sit at about +-34 / 180+-34
		// degrees, so every button stays clear of them. Caps (big targets): A / B at the
		// bottom under the thumbs, >> between them; L / R shoulders on top, M between them.
		// Crescents at the sides (rarely used): START right, SELECT left.
		add_arc(36, 70, KEY_A, "A");
		add_arc(110, 144, KEY_B, "B");
		add_arc(76, 104, VIEW_FAST, ">>");
		add_arc(216, 250, KEY_L, "L");
		add_arc(290, 324, KEY_R, "R");
		add_arc(256, 284, VIEW_MODE, "M");
		add_arc(-24, 24, KEY_START, "ST");
		add_arc(156, 204, KEY_SELECT, "SE");
	} else {
		float u = (float) (w < h ? w : h);
		add_btn(w - 0.17f * u, h * 0.58f, 0.10f * u, KEY_A, "A");
		add_btn(w - 0.38f * u, h * 0.74f, 0.085f * u, KEY_B, "B");
		add_btn(w - 0.14f * u, h - 0.10f * u, 0.06f * u, KEY_START, "ST");
		add_btn(0.14f * u, h - 0.10f * u, 0.06f * u, KEY_SELECT, "SE");
		add_btn(0.12f * u, 0.11f * u, 0.08f * u, KEY_L, "L");
		add_btn(w - 0.12f * u, 0.11f * u, 0.08f * u, KEY_R, "R");
		add_btn(w * 0.5f - 0.08f * u, 0.07f * u, 0.05f * u, VIEW_MODE, "M");
		add_btn(w * 0.5f + 0.08f * u, 0.07f * u, 0.05f * u, VIEW_FAST, ">>");
	}
}

// Half size of the undistorted picture on a round screen (zoom 1).
static void img_half(float* hw, float* hh) {
	float d = sqrtf((float) (EMU_W * EMU_W + EMU_H * EMU_H));
	float ri = V.R * IMG_RHO;
	*hw = ri * EMU_W / d;
	*hh = ri * EMU_H / d;
}

// Distance from the center to the picture border along the angle (degrees, y-down).
static float border_dist(float deg) {
	float hw, hh;
	img_half(&hw, &hh);
	float a = DEG(deg);
	float c = fabsf(cosf(a)), s = fabsf(sinf(a));
	float dx = c > 1e-6f ? hw / c : 1e9f;
	float dy = s > 1e-6f ? hh / s : 1e9f;
	return dx < dy ? dx : dy;
}

// Inner radius of a button at this angle: just outside the picture, never past the rim.
static float btn_inner(float deg) {
	return SDL_min(border_dist(deg) + BTN_GAP * V.R, ARC_OUT * V.R - 2.0f);
}

static bool angle_in(float deg, float a0, float a1) {
	float span = a1 - a0;
	float d = fmodf(deg - a0, 360.0f);
	if (d < 0) {
		d += 360.0f;
	}
	return d <= span;
}

uint32_t view_hit(float x, float y) {
	if (!V.overlay) {
		return 0;
	}
	float dx = x - V.cx, dy = y - V.cy;
	float dist = sqrtf(dx * dx + dy * dy);
	float deg = atan2f(dy, dx) * 180.0f / PI_F;
	for (int i = 0; i < V.nbtn; ++i) {
		const Btn* b = &V.btn[i];
		if (b->arc) {
			// Anywhere outside the picture within the button's angles (plus the gaps between
			// buttons): the whole cap or crescent is the target, the picture stays game input.
			if (dist >= border_dist(deg) + 0.5f * BTN_GAP * V.R && angle_in(deg, b->a0 - 3, b->a1 + 3)) {
				return b->bits;
			}
		} else {
			float bx = x - b->cx, by = y - b->cy;
			float r = b->r * 1.25f;  // generous touch slop
			if (bx * bx + by * by <= r * r) {
				return b->bits;
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------- primitives
#define SEG 40

static void fill_circle(SDL_Renderer* r, float cx, float cy, float rad, SDL_FColor c) {
	SDL_Vertex v[SEG + 1];
	int idx[SEG * 3];
	v[0] = (SDL_Vertex) { { cx, cy }, c, { 0, 0 } };
	for (int i = 0; i < SEG; ++i) {
		float a = i * 6.2831853f / SEG;
		v[i + 1] = (SDL_Vertex) { { cx + cosf(a) * rad, cy + sinf(a) * rad }, c, { 0, 0 } };
		idx[i * 3] = 0;
		idx[i * 3 + 1] = i + 1;
		idx[i * 3 + 2] = (i + 1) % SEG + 1;
	}
	SDL_RenderGeometry(r, NULL, v, SEG + 1, idx, SEG * 3);
}

// Ring segment between radii r0..r1 and angles a0..a1 (degrees). Full ring if a1 - a0 >= 360.
static void arc_band(SDL_Renderer* r, float cx, float cy, float r0, float r1, float a0, float a1, SDL_FColor c) {
	enum { N = 40 };
	SDL_Vertex v[(N + 1) * 2];
	int idx[N * 6];
	for (int i = 0; i <= N; ++i) {
		float a = DEG(a0 + (a1 - a0) * i / N);
		float ca = cosf(a), sa = sinf(a);
		v[i * 2] = (SDL_Vertex) { { cx + ca * r0, cy + sa * r0 }, c, { 0, 0 } };
		v[i * 2 + 1] = (SDL_Vertex) { { cx + ca * r1, cy + sa * r1 }, c, { 0, 0 } };
	}
	for (int i = 0; i < N; ++i) {
		int* q = &idx[i * 6];
		q[0] = i * 2; q[1] = i * 2 + 1; q[2] = i * 2 + 2;
		q[3] = i * 2 + 2; q[4] = i * 2 + 1; q[5] = i * 2 + 3;
	}
	SDL_RenderGeometry(r, NULL, v, (N + 1) * 2, idx, N * 6);
}

static void ring(SDL_Renderer* r, float cx, float cy, float r0, float r1, SDL_FColor c) {
	arc_band(r, cx, cy, r0, r1, 0, 360, c);
}

// Round-screen button: from just outside the picture border out to the rim.
static void btn_band(SDL_Renderer* r, float a0, float a1, SDL_FColor c) {
	enum { N = 32 };
	SDL_Vertex v[(N + 1) * 2];
	int idx[N * 6];
	float ro = ARC_OUT * V.R;
	for (int i = 0; i <= N; ++i) {
		float deg = a0 + (a1 - a0) * i / N;
		float a = DEG(deg);
		float ca = cosf(a), sa = sinf(a), ri = btn_inner(deg);
		v[i * 2] = (SDL_Vertex) { { V.cx + ca * ri, V.cy + sa * ri }, c, { 0, 0 } };
		v[i * 2 + 1] = (SDL_Vertex) { { V.cx + ca * ro, V.cy + sa * ro }, c, { 0, 0 } };
	}
	for (int i = 0; i < N; ++i) {
		int* q = &idx[i * 6];
		q[0] = i * 2; q[1] = i * 2 + 1; q[2] = i * 2 + 2;
		q[3] = i * 2 + 2; q[4] = i * 2 + 1; q[5] = i * 2 + 3;
	}
	SDL_RenderGeometry(r, NULL, v, (N + 1) * 2, idx, N * 6);
}

static void text_centered(SDL_Renderer* r, float cx, float cy, float px, const char* s, SDL_FColor c) {
	// Whole-number scale and pixel-aligned origin: the 8x8 font stays sharp (a fractional
	// scale smears it into uneven, blurry strokes).
	float k = floorf(px / (float) SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
	if (k < 1) {
		k = 1;
	}
	float tw = (float) strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
	SDL_SetRenderScale(r, k, k);
	SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
	SDL_RenderDebugText(r, roundf(cx / k - tw * 0.5f), roundf(cy / k - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 0.5f), s);
	SDL_SetRenderScale(r, 1, 1);
}

static SDL_Texture* crisp_source(SDL_Renderer* r, SDL_Texture* game) {
	if (V.smooth) {
		SDL_SetTextureScaleMode(game, SDL_SCALEMODE_LINEAR);
		return game;
	}
	if (!V.up) {
		V.up = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, EMU_W * 3, EMU_H * 3);
		if (!V.up) {
			SDL_SetTextureScaleMode(game, SDL_SCALEMODE_NEAREST);
			return game;
		}
		SDL_SetTextureBlendMode(V.up, SDL_BLENDMODE_NONE);
	}
	// Nearest 3x upscale first, then the final scale samples it bilinearly: sharp pixels
	// without the uneven pixel widths of a plain nearest scale by a fractional factor.
	SDL_SetTextureScaleMode(game, SDL_SCALEMODE_NEAREST);
	SDL_SetTextureBlendMode(game, SDL_BLENDMODE_NONE);
	SDL_SetRenderTarget(r, V.up);
	SDL_RenderTexture(r, game, NULL, NULL);
	SDL_SetRenderTarget(r, NULL);
	SDL_SetTextureScaleMode(V.up, SDL_SCALEMODE_LINEAR);
	return V.up;
}

// Undistorted picture: the inscribed 3:2 rectangle, optionally zoomed around the center.
static void render_round(SDL_Renderer* r, SDL_Texture* game) {
	float target = PRESET_ZOOM[V.screen];
	if (V.zoomCur < 1.0f) {
		V.zoomCur = target;
	}
	V.zoomCur += SDL_clamp(target - V.zoomCur, -0.02f, 0.02f);
	float hw, hh;
	img_half(&hw, &hh);
	hw *= V.zoomCur;
	hh *= V.zoomCur;
	SDL_FRect dst = { roundf(V.cx - hw), roundf(V.cy - hh), roundf(2.0f * hw), roundf(2.0f * hh) };
	SDL_RenderTexture(r, crisp_source(r, game), NULL, &dst);
}

// ---------------------------------------------------------------- render

static SDL_FColor faded(SDL_FColor c, float k) {
	c.a *= k;
	return c;
}

static void render_stick(SDL_Renderer* r, const TouchStick* st) {
	ring(r, st->ax, st->ay, st->radius - 2, st->radius, C_STICK);
	// Direction ticks (rotated 45 degrees in isometric mode); the active one lights up.
	static const struct { float deg; uint16_t bit; } dirs[] = {
		{ 0, KEY_RIGHT }, { 90, KEY_DOWN }, { 180, KEY_LEFT }, { 270, KEY_UP },
	};
	float off = input_iso() ? -45.0f : 0.0f;
	for (int i = 0; i < 4; ++i) {
		float a = dirs[i].deg + off;
		bool on = st->dir == dirs[i].bit;
		arc_band(r, st->ax, st->ay, st->radius + 3, st->radius + (on ? 12 : 7), a - 14, a + 14, on ? C_BTN_ON : C_STICK);
	}
	fill_circle(r, st->x, st->y, st->radius * 0.30f, C_KNOB);
}

static void render_overlay(SDL_Renderer* r) {
	uint32_t held = input_touch_held() | input_gesture_keys();
	if (V.fast) {
		held |= VIEW_FAST;
	}
	uint64_t now = SDL_GetTicksNS();
	const TouchStick* st = input_stick();
	if (input_touch_held() || st->active || V.lastTouchNs == 0) {
		V.lastTouchNs = now;
	}
	// Controls dim after a few seconds without touches so the picture stays clean.
	float k = now - V.lastTouchNs > IDLE_FADE_NS ? 0.5f : 1.0f;

	for (int i = 0; i < V.nbtn; ++i) {
		const Btn* b = &V.btn[i];
		bool on = (held & b->bits) != 0;
		if (b->arc) {
			btn_band(r, b->a0, b->a1, on ? C_BTN_ON : faded(C_ARC, k));
			float midDeg = (b->a0 + b->a1) * 0.5f;
			float mid = DEG(midDeg);
			float ri = btn_inner(midDeg), ro = ARC_OUT * V.R;
			float lr = (ri + ro) * 0.5f;
			// 8 px font at 1x-3x: 3x in the big caps, 2x in the side crescents.
			float px = SDL_clamp((ro - ri) * 0.55f, 8.0f, 24.0f);
			float lx = V.cx + cosf(mid) * lr, ly = V.cy + sinf(mid) * lr;
			SDL_FColor lc = faded(C_LABEL, on ? 1.0f : 0.75f * k);
			float glyph = floorf(SDL_max(px, 8.0f) / 8.0f) * 8.0f;
			size_t n = strlen(b->label);
			if (n > 1 && glyph * n > (ro - ri) * 0.85f) {
				// Narrow side crescent: stack the letters so the label stays inside it.
				char one[2] = { 0, 0 };
				float step = glyph * 1.15f;
				for (size_t c = 0; c < n; ++c) {
					one[0] = b->label[c];
					text_centered(r, lx, ly + (c - (n - 1) * 0.5f) * step, px, one, lc);
				}
			} else {
				text_centered(r, lx, ly, px, b->label, lc);
			}
		} else {
			fill_circle(r, b->cx, b->cy, b->r, on ? C_BTN_ON : faded(C_BTN, k));
			ring(r, b->cx, b->cy, b->r - SDL_max(1.5f, b->r * 0.08f), b->r, faded(C_BTN_EDGE, k));
			text_centered(r, b->cx, b->cy, b->r * 0.6f, b->label, faded(C_LABEL, k));
		}
	}
	if (st->active) {
		render_stick(r, st);
	}
}

static SDL_FRect game_rect(void) {
	float sx = (float) V.w / EMU_W, sy = (float) V.h / EMU_H;
	float s = sx < sy ? sx : sy;
	float gw = EMU_W * s, gh = EMU_H * s;
	return (SDL_FRect) { V.cx - gw * 0.5f, V.cy - gh * 0.5f, gw, gh };
}

// Toast: 16 px glyphs (8 px on tiny screens), split over two lines when one does not fit.
static void render_toast(SDL_Renderer* r, float y, float maxW, float alpha) {
	const float g = V.R >= 150.0f ? 16.0f : 8.0f;
	const char* msg = V.toast;
	size_t n = strlen(msg);
	char line[2][96];
	int lines = 1;
	SDL_strlcpy(line[0], msg, sizeof(line[0]));
	line[1][0] = 0;
	if (n * g > maxW) {
		size_t best = 0;
		for (size_t c = 1; c < n; ++c) {
			if (msg[c] == ' ' && (best == 0 || SDL_abs((int) c - (int) n / 2) < SDL_abs((int) best - (int) n / 2))) {
				best = c;
			}
		}
		if (best) {
			SDL_strlcpy(line[0], msg, best + 1);
			SDL_strlcpy(line[1], msg + best + 1, sizeof(line[1]));
			lines = 2;
		}
	}
	size_t widest = SDL_max(strlen(line[0]), strlen(line[1]));
	float tw = (float) widest * g, step = g * 1.35f;
	float th = step * (float) lines + g * 0.5f;
	SDL_FRect bg = { roundf(V.cx - tw * 0.5f - g * 0.6f), roundf(y - th * 0.5f), roundf(tw + g * 1.2f), roundf(th) };
	SDL_SetRenderDrawColorFloat(r, C_TOAST_BG.r, C_TOAST_BG.g, C_TOAST_BG.b, C_TOAST_BG.a * alpha);
	SDL_RenderFillRect(r, &bg);
	for (int l = 0; l < lines; ++l) {
		float ly = y + ((float) l - (lines - 1) * 0.5f) * step;
		text_centered(r, V.cx, ly, g, line[l], faded(C_LABEL, alpha));
	}
}

void view_render(SDL_Renderer* r, SDL_Texture* game) {
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
	SDL_RenderClear(r);

	if (V.round) {
		render_round(r, game);
	} else {
		SDL_Texture* src = crisp_source(r, game);
		SDL_FRect dst = game_rect();
		SDL_RenderTexture(r, src, NULL, &dst);
	}

	if (V.overlay && (V.round || input_touch_seen())) {
		render_overlay(r);
	}

	if (V.round && V.mask) {
		float far = sqrtf((float) (V.w * V.w + V.h * V.h));
		ring(r, V.cx, V.cy, V.R, far, C_MASK);
	}

	uint64_t now = SDL_GetTicksNS();
	if (V.toast[0] && now - V.toastNs < TOAST_NS) {
		float t = (float) (now - V.toastNs) / (float) TOAST_NS;
		float alpha = t > 0.8f ? (1.0f - t) / 0.2f : 1.0f;
		float y = V.h * 0.18f, maxW = V.w * 0.9f;
		if (V.round) {
			// In the top cap above the picture, never over the game's own text.
			float hw, hh;
			img_half(&hw, &hh);
			y = V.cy - (hh * V.zoomCur + V.R) * 0.5f;
			float dy = y - V.cy;
			maxW = 2.0f * sqrtf(SDL_max(0.0f, V.R * V.R - dy * dy)) * 0.86f;
		}
		render_toast(r, y, maxW, alpha);
	}

	if (V.debug[0]) {
		float px = SDL_max(8.0f, V.R * 0.05f);
		float y = V.round ? V.cy + V.R * 0.55f : V.h - px * 1.5f;
		text_centered(r, V.cx, y, px, V.debug, C_LABEL);
	}
}
