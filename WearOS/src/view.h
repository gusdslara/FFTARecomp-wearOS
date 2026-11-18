// Presentation: game image placement (round / rect screens) + touch overlay.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

// Overlay-only buttons (outside the 10 GBA key bits).
enum {
	VIEW_MODE = 1 << 12,  // tap: screen preset / hold: isometric drag
	VIEW_FAST = 1 << 13,  // tap: fast-forward on/off
};

// Round-screen presets. Never distorted: the picture is only ever scaled evenly.
typedef enum {
	SCREEN_FULL = 0,   // largest inscribed 3:2 rectangle: the whole 240x160 frame visible
	SCREEN_ZOOM,       // 1.15x around the center, edges go under the buttons
	SCREEN_ZOOM_MORE,  // 1.30x around the center
	SCREEN_COUNT
} ScreenPreset;

void view_layout(int w, int h, bool round);
uint32_t view_hit(float x, float y);

void view_set_screen(ScreenPreset p);
ScreenPreset view_screen(void);
const char* view_screen_name(ScreenPreset p);

void view_set_overlay(bool on);
bool view_overlay(void);
void view_set_mask(bool on);  // draw circular mask (desktop round simulation)
void view_set_smooth(bool on);
bool view_smooth(void);
void view_set_fast(bool on);  // lights the >> button

void view_toast(const char* msg);
void view_set_debug(const char* text);  // NULL hides the debug line

void view_render(SDL_Renderer* r, SDL_Texture* game);
