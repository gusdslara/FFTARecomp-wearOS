// Final Fantasy Tactics Advance — native watch shell, SDL3 app callbacks
// (desktop, Android phone, Wear OS). The emulation core is libmgba; the ROM is the user's own.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "emu.h"
#include "input.h"
#include "view.h"

#define APP_ORG "wearosports"
#define APP_NAME "ffta"
#define AUDIO_MAX_QUEUE_FRAMES (EMU_AUDIO_RATE / 10)  // 100 ms
#define FAST_MULT 3.0

static struct {
	SDL_Window* win;
	SDL_Renderer* ren;
	SDL_Texture* tex;
	SDL_AudioStream* audio;
	bool running;  // ROM loaded
	bool paused;
	bool fastHold;    // Tab (desktop)
	bool fastToggle;  // >> button
	bool showDebug;
	bool round;
	bool pattern;
	bool selftest;
	int testFrames;   // --test N: scripted run + screenshots, then quit
	int frameNo;
	char err[512];
	char base[1024];
	char romPath[1024];
	char savePath[1024];
	char statePath[1024];
	char autoPath[1024];
	char cfgPath[1024];
	uint64_t lastNs;
	double accNs;
	double frameNs;
	int fpsFrames;
	uint64_t fpsStartNs;
	double fpsValue;
	float wheelAcc;
	// Android delivers lifecycle events on its UI thread while the main thread may be
	// inside a frame: the core is only touched under this lock.
	SDL_Mutex* coreLock;
} A;

// ---------------------------------------------------------------- settings

static void cfg_save(void) {
	char buf[64];
	int n = SDL_snprintf(buf, sizeof(buf), "view=%d\niso=%d\n", (int) view_screen(), input_iso() ? 1 : 0);
	SDL_SaveFile(A.cfgPath, buf, (size_t) n);
}

static void cfg_load(void) {
	size_t size = 0;
	char* s = SDL_LoadFile(A.cfgPath, &size);
	if (!s) {
		return;
	}
	// "view=" replaced "screen=" when the lens presets went away: old values meant a
	// distorted lens, so they are ignored and the undistorted full picture is used.
	int screen = SCREEN_FULL, iso = 0;
	const char* p = SDL_strstr(s, "view=");
	if (p) screen = SDL_atoi(p + 5);
	p = SDL_strstr(s, "iso=");
	if (p) iso = SDL_atoi(p + 4);
	view_set_screen((ScreenPreset) screen);
	input_set_iso(iso != 0);
	SDL_free(s);
}

// ---------------------------------------------------------------- helpers

static void relayout(void) {
	int w, h;
	SDL_GetRenderOutputSize(A.ren, &w, &h);
	view_layout(w, h, A.round);
	input_set_screen(w, h);
}

static bool file_exists(const char* p) {
	SDL_PathInfo info;
	return p[0] && SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE;
}

static void usage(void) {
	SDL_Log("uso: ffta [rom.gba] [--round] [--size WxH] [--smooth] [--fresh]");
	SDL_Log("teclas: setas/WASD  Z=A X=B Enter=Start Backspace=Select Q=L E=R  roda=cima/baixo");
	SDL_Log("        F3 tela  F4 overlay  F5 salvar estado  F7 carregar estado  F6 filtro");
	SDL_Log("        F8 arrasto isometrico  F9 debug  Tab turbo (segurar)  P pausa  F11 tela cheia");
}

static void set_paused(bool p) {
	A.paused = p;
	if (A.audio) {
		if (p) {
			SDL_PauseAudioStreamDevice(A.audio);
			SDL_ClearAudioStream(A.audio);
		} else {
			SDL_ResumeAudioStreamDevice(A.audio);
		}
	}
	A.lastNs = SDL_GetTicksNS();
	A.accNs = 0;
}

// Watches get killed in the background all the time: keep a resume point.
static void auto_save(void) {
	if (A.running && !A.testFrames) {
		SDL_LockMutex(A.coreLock);
		emu_save_state(A.autoPath);
		SDL_UnlockMutex(A.coreLock);
	}
}

static void cycle_screen(void) {
	view_set_screen((ScreenPreset) ((view_screen() + 1) % SCREEN_COUNT));
	view_toast(view_screen_name(view_screen()));
	cfg_save();
}

static void toggle_iso(void) {
	input_set_iso(!input_iso());
	view_toast(input_iso() ? "Arrasto: diagonal (mapa)" : "Arrasto: reto");
	cfg_save();
}

static void toggle_fast(void) {
	A.fastToggle = !A.fastToggle;
	view_set_fast(A.fastToggle);
	view_toast(A.fastToggle ? "Velocidade 3x" : "Velocidade normal");
}

// ---------------------------------------------------------------- app

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
	SDL_SetAppMetadata("Final Fantasy Tactics Advance", "0.1.0", "com.wearosports.ffta");
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  // mouse simulates a finger on desktop
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

	int winW = 720, winH = 480;
	const char* romArg = NULL;
	bool fresh = false;
	for (int i = 1; i < argc; ++i) {
		if (!SDL_strcmp(argv[i], "--round")) {
			A.round = true;
			winW = winH = 454;
		} else if (!SDL_strcmp(argv[i], "--size") && i + 1 < argc) {
			SDL_sscanf(argv[++i], "%dx%d", &winW, &winH);
		} else if (!SDL_strcmp(argv[i], "--test") && i + 1 < argc) {
			A.testFrames = SDL_atoi(argv[++i]);
		} else if (!SDL_strcmp(argv[i], "--smooth")) {
			view_set_smooth(true);
		} else if (!SDL_strcmp(argv[i], "--pattern")) {
			A.pattern = true;
		} else if (!SDL_strcmp(argv[i], "--selftest")) {
			A.selftest = true;
		} else if (!SDL_strcmp(argv[i], "--fresh")) {
			fresh = true;
		} else if (!SDL_strcmp(argv[i], "--help")) {
			usage();
			return SDL_APP_SUCCESS;
		} else if (argv[i][0] != '-') {
			romArg = argv[i];
		}
	}

	A.coreLock = SDL_CreateMutex();
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		SDL_Log("SDL_Init: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_ANDROID)
	wf |= SDL_WINDOW_FULLSCREEN;
#endif
	if (!SDL_CreateWindowAndRenderer("Final Fantasy Tactics Advance", winW, winH, wf, &A.win, &A.ren)) {
		SDL_Log("janela: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	SDL_SetRenderVSync(A.ren, A.testFrames ? 0 : 1);
	A.tex = SDL_CreateTexture(A.ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, EMU_W, EMU_H);

	view_set_mask(A.round);
	input_init();
	input_set_hit_fn(view_hit);
	relayout();

	// Paths: ROM from the argument (Android passes its private copy), saves in the pref dir.
	char* pref = SDL_GetPrefPath(APP_ORG, APP_NAME);
	SDL_strlcpy(A.base, pref ? pref : "./", sizeof(A.base));
	SDL_free(pref);
	SDL_snprintf(A.savePath, sizeof(A.savePath), "%sffta.sav", A.base);
	SDL_snprintf(A.statePath, sizeof(A.statePath), "%sffta.ss1", A.base);
	SDL_snprintf(A.autoPath, sizeof(A.autoPath), "%sffta.auto.ss", A.base);
	SDL_snprintf(A.cfgPath, sizeof(A.cfgPath), "%sffta.cfg", A.base);
	cfg_load();

	if (romArg) {
		SDL_strlcpy(A.romPath, romArg, sizeof(A.romPath));
	} else {
		SDL_snprintf(A.romPath, sizeof(A.romPath), "%srom.gba", A.base);
		if (!file_exists(A.romPath) && file_exists("rom.gba")) {
			SDL_strlcpy(A.romPath, "rom.gba", sizeof(A.romPath));
		}
	}

	if (emu_init(A.romPath, A.testFrames ? NULL : A.savePath, A.err, sizeof(A.err))) {
		A.running = true;
		A.frameNs = 1e9 / emu_fps();
		char title[13], code[5];
		emu_rom_id(title, code);
		SDL_Log("ROM: %s [%s/%s] (%.4f fps)  save: %s", A.romPath, title, code, emu_fps(), A.savePath);
		if (SDL_strncmp(code, "AFX", 3) != 0) {
			view_toast("Aviso: ROM nao parece ser FFTA");
		}
		if (!fresh && !A.testFrames && file_exists(A.autoPath) && emu_load_state(A.autoPath)) {
			view_toast("Continuando de onde parou");
		}
	} else {
		SDL_Log("%s", A.err);
		SDL_Log("Copie a ROM para: %srom.gba", A.base);
	}

	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, EMU_AUDIO_RATE };
	A.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (A.audio) {
		SDL_ResumeAudioStreamDevice(A.audio);
	} else {
		SDL_Log("audio: %s", SDL_GetError());
	}

	usage();
	A.lastNs = A.fpsStartNs = SDL_GetTicksNS();
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* e) {
	input_handle_event(e);
	switch (e->type) {
	case SDL_EVENT_QUIT:
	case SDL_EVENT_TERMINATING:
		auto_save();
		return SDL_APP_SUCCESS;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
		relayout();
		break;
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
		if (!A.paused) {
			auto_save();
		}
		set_paused(true);
		break;
	case SDL_EVENT_DID_ENTER_BACKGROUND:
		set_paused(true);
		break;
	case SDL_EVENT_DID_ENTER_FOREGROUND:
		set_paused(false);
		break;
	case SDL_EVENT_MOUSE_WHEEL: {
		// Rotary crown / bezel / mouse wheel: one D-pad up/down press per detent.
		float y = e->wheel.y;
#if !defined(SDL_PLATFORM_ANDROID)
		y = -y;  // desktop: wheel towards the user = down the list
#endif
		A.wheelAcc += y;
		int det = (int) A.wheelAcc;
		if (det) {
			A.wheelAcc -= (float) det;
			input_crown(det);
		}
		break;
	}
	case SDL_EVENT_KEY_DOWN:
		if (e->key.repeat) {
			break;
		}
		switch (e->key.key) {
		case SDLK_F3: cycle_screen(); break;
		case SDLK_F4: view_set_overlay(!view_overlay()); break;
		case SDLK_F5:
			if (A.running) view_toast(emu_save_state(A.statePath) ? "Estado salvo" : "Falha ao salvar estado");
			break;
		case SDLK_F7:
			if (A.running) view_toast(emu_load_state(A.statePath) ? "Estado carregado" : "Sem estado salvo");
			break;
		case SDLK_F6:
			view_set_smooth(!view_smooth());
			view_toast(view_smooth() ? "Filtro: suave" : "Filtro: pixel");
			break;
		case SDLK_F8: toggle_iso(); break;
		case SDLK_F9:
			A.showDebug = !A.showDebug;
			if (!A.showDebug) view_set_debug(NULL);
			break;
		case SDLK_F11: {
			bool fs = (SDL_GetWindowFlags(A.win) & SDL_WINDOW_FULLSCREEN) != 0;
			SDL_SetWindowFullscreen(A.win, !fs);
			break;
		}
		case SDLK_P: set_paused(!A.paused); view_toast(A.paused ? "Pausado" : "Continuar"); break;
		case SDLK_TAB: A.fastHold = true; break;
		case SDLK_ESCAPE:
#ifndef SDL_PLATFORM_ANDROID
			auto_save();
			return SDL_APP_SUCCESS;
#else
			break;
#endif
		default: break;
		}
		break;
	case SDL_EVENT_KEY_UP:
		if (e->key.key == SDLK_TAB) A.fastHold = false;
		break;
	default:
		break;
	}
	uint32_t act = input_take_actions();
	if (act & ACT_SCREEN) cycle_screen();
	if (act & ACT_ISO) toggle_iso();
	if (act & ACT_FAST) toggle_fast();
	return SDL_APP_CONTINUE;
}

static void run_emulation(void) {
	uint64_t now = SDL_GetTicksNS();
	double dt = (double) (now - A.lastNs);
	A.lastNs = now;
	if (A.paused) {
		return;
	}
	bool fast = A.fastHold || A.fastToggle;
	double mult = A.fastHold ? 4.0 : A.fastToggle ? FAST_MULT : 1.0;
	A.accNs += dt * mult;
	if (A.accNs > A.frameNs * 6 * mult) {
		A.accNs = A.frameNs;  // fell far behind (slow watch CPU, suspend): don't spiral
	}

	static int16_t samples[4096 * 2];
	int ran = 0;
	int maxRun = fast ? 8 : 3;
	uint64_t budget = SDL_GetTicksNS() + 14000000ull;  // keep the UI responsive on slow CPUs
	SDL_LockMutex(A.coreLock);
	while (A.accNs >= A.frameNs && ran < maxRun) {
		A.accNs -= A.frameNs;
		emu_run_frame(input_frame());
		++ran;
		int n = emu_read_audio(samples, 4096);
		if (A.audio && n > 0 && !fast) {
			int queued = SDL_GetAudioStreamQueued(A.audio) / 4;
			if (queued < AUDIO_MAX_QUEUE_FRAMES) {
				SDL_PutAudioStreamData(A.audio, samples, n * 4);
			}
		}
		if (fast && SDL_GetTicksNS() > budget) {
			A.accNs = 0;  // the CPU can't keep the multiplier: run as fast as it can
			break;
		}
	}
	SDL_UnlockMutex(A.coreLock);
	if (ran) {
		SDL_UpdateTexture(A.tex, NULL, emu_framebuffer(), EMU_W * 4);
		A.fpsFrames += ran;
	}
	if (now - A.fpsStartNs >= 1000000000ull) {
		A.fpsValue = A.fpsFrames * 1e9 / (double) (now - A.fpsStartNs);
		A.fpsFrames = 0;
		A.fpsStartNs = now;
	}
	if (A.showDebug) {
		uint16_t dc = emu_dispcnt();
		char buf[128];
		SDL_snprintf(buf, sizeof(buf), "%.1ffps mode%d iso%d", A.fpsValue, dc & 7, input_iso());
		view_set_debug(buf);
	}
}

// ---------------------------------------------------------------- --test (desktop verification)

// Scripted run: mash through the intro / title, then exercise the cursor.
static uint16_t test_keys(int f) {
	if (f < 3000) {
		return f % 60 < 3 ? ((f / 60) % 3 ? KEY_A : KEY_START) : 0;
	}
	static const uint16_t cyc[] = { KEY_UP, KEY_RIGHT, KEY_DOWN, KEY_LEFT };
	return f % 40 < 4 ? cyc[(f / 160) % 4] : 0;
}

static void test_shot(void) {
	SDL_Surface* s = SDL_RenderReadPixels(A.ren, NULL);
	if (s) {
		char path[256];
		SDL_snprintf(path, sizeof(path), "shots/f%05d.bmp", A.frameNo);
		SDL_SaveBMP(s, path);
		SDL_DestroySurface(s);
	}
	SDL_Surface* raw = SDL_CreateSurfaceFrom(EMU_W, EMU_H, SDL_PIXELFORMAT_XBGR8888, (void*) emu_framebuffer(), EMU_W * 4);
	if (raw) {
		char path[256];
		SDL_snprintf(path, sizeof(path), "shots/raw%05d.bmp", A.frameNo);
		SDL_SaveBMP(raw, path);
		SDL_DestroySurface(raw);
	}
	uint16_t dc = emu_dispcnt();
	SDL_Log("frame %d DISPCNT=%04X mode=%d", A.frameNo, dc, dc & 7);
}

static SDL_AppResult test_iterate(void) {
	static int16_t samples[4096 * 2];
	bool shot = false;
	for (int i = 0; i < 20 && A.frameNo < A.testFrames; ++i) {
		emu_run_frame(test_keys(A.frameNo) | input_frame());
		emu_read_audio(samples, 4096);
		++A.frameNo;
		if (A.frameNo % 300 == 0) {
			shot = true;
			break;
		}
	}
	SDL_UpdateTexture(A.tex, NULL, emu_framebuffer(), EMU_W * 4);
	view_render(A.ren, A.tex);
	if (shot) {
		test_shot();
	}
	SDL_RenderPresent(A.ren);
	return A.frameNo >= A.testFrames ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

// --pattern: renders a 240x160 test card (1 px border, corner blocks, 8 px grid) through every
// screen preset and saves shots/pattern_<n>.bmp. Verifies nothing is cropped on round screens.
static SDL_AppResult pattern_iterate(void) {
	static uint32_t px[EMU_W * EMU_H];
	for (int y = 0; y < EMU_H; ++y) {
		for (int x = 0; x < EMU_W; ++x) {
			uint32_t c = ((x / 8 + y / 8) & 1) ? 0xFF303848 : 0xFF202830;  // XBGR
			if (x % 40 == 0 || y % 40 == 0) c = 0xFF708090;
			bool cx = x < 12 || x >= EMU_W - 12, cy = y < 12 || y >= EMU_H - 12;
			if (cx && cy) c = 0xFF00C0FF;                                       // corners: orange
			if (x == 0 || y == 0 || x == EMU_W - 1 || y == EMU_H - 1) c = 0xFF00FF00;  // border: green
			px[y * EMU_W + x] = c;
		}
	}
	SDL_UpdateTexture(A.tex, NULL, px, EMU_W * 4);
	for (int p = 0; p < SCREEN_COUNT; ++p) {
		view_set_screen((ScreenPreset) p);
		for (int i = 0; i < 40; ++i) {
			view_render(A.ren, A.tex);  // let the zoom animation settle
		}
		SDL_Surface* s = SDL_RenderReadPixels(A.ren, NULL);
		if (s) {
			char path[256];
			SDL_snprintf(path, sizeof(path), "shots/pattern_%d.bmp", p);
			SDL_SaveBMP(s, path);
			SDL_DestroySurface(s);
		}
		SDL_RenderPresent(A.ren);
	}
	return SDL_APP_SUCCESS;
}

// --selftest: feeds synthetic touches through the real input code and checks the GBA keys.
static void st_touch(SDL_EventType t, SDL_FingerID f, float x, float y) {
	int w, h;
	SDL_GetRenderOutputSize(A.ren, &w, &h);
	SDL_Event e;
	SDL_zero(e);
	e.type = t;
	e.tfinger.touchID = 1;
	e.tfinger.fingerID = f;
	e.tfinger.x = x / (float) w;
	e.tfinger.y = y / (float) h;
	input_handle_event(&e);
}

static uint16_t st_frames(int n) {
	uint16_t acc = 0;
	for (int i = 0; i < n; ++i) {
		acc |= input_frame();
	}
	return acc;
}

static int st_check(const char* name, uint16_t got, uint16_t want) {
	SDL_Log("%-34s keys=%03X want=%03X %s", name, got, want, got == want ? "OK" : "FALHOU");
	return got == want ? 0 : 1;
}

static SDL_AppResult selftest(void) {
	int w, h, fails = 0;
	SDL_GetRenderOutputSize(A.ren, &w, &h);
	float cx = w * 0.5f, cy = h * 0.5f, R = (w < h ? w : h) * 0.5f;
	st_frames(10);

	st_touch(SDL_EVENT_FINGER_DOWN, 1, cx, cy);
	st_touch(SDL_EVENT_FINGER_UP, 1, cx + 2, cy);
	fails += st_check("toque rapido = A", st_frames(8), KEY_A);

	st_touch(SDL_EVENT_FINGER_DOWN, 1, cx - 20, cy);
	st_touch(SDL_EVENT_FINGER_DOWN, 2, cx + 20, cy);
	st_touch(SDL_EVENT_FINGER_UP, 2, cx + 20, cy);
	st_touch(SDL_EVENT_FINGER_UP, 1, cx - 20, cy);
	fails += st_check("toque com 2 dedos = B", st_frames(14), KEY_B);

	const struct { const char* n; float dx, dy; uint16_t want, wantIso; } drags[] = {
		{ "arrasto direita", 1, 0, KEY_RIGHT, KEY_RIGHT },  // iso: exact axis is a sector edge, see below
		{ "arrasto cima", 0, -1, KEY_UP, KEY_UP },
		{ "arrasto diag. cima-direita (iso)", 0.7f, -0.7f, 0, KEY_UP },
		{ "arrasto diag. baixo-esquerda (iso)", -0.7f, 0.7f, 0, KEY_DOWN },
	};
	for (int iso = 0; iso < 2; ++iso) {
		input_set_iso(iso != 0);
		for (int i = 0; i < 4; ++i) {
			if (!iso && drags[i].want == 0) continue;
			if (iso && i < 2) continue;
			st_touch(SDL_EVENT_FINGER_DOWN, 3, cx, cy);
			for (int s = 1; s <= 6; ++s) {
				st_touch(SDL_EVENT_FINGER_MOTION, 3, cx + drags[i].dx * R * 0.08f * s, cy + drags[i].dy * R * 0.08f * s);
			}
			uint16_t k = st_frames(5);
			st_touch(SDL_EVENT_FINGER_UP, 3, cx, cy);
			st_frames(6);
			fails += st_check(drags[i].n, k, iso ? drags[i].wantIso : drags[i].want);
		}
	}
	input_set_iso(false);

	st_touch(SDL_EVENT_FINGER_DOWN, 4, cx, cy);
	for (int s = 1; s <= 6; ++s) st_touch(SDL_EVENT_FINGER_MOTION, 4, cx, cy + R * 0.08f * s);
	st_frames(3);
	st_touch(SDL_EVENT_FINGER_DOWN, 5, cx + 60, cy - 60);
	st_touch(SDL_EVENT_FINGER_UP, 5, cx + 60, cy - 60);
	fails += st_check("arrasto baixo + toque 2o dedo", st_frames(8), KEY_DOWN | KEY_A);
	st_touch(SDL_EVENT_FINGER_UP, 4, cx, cy);
	st_frames(6);

	// A button: bottom-right cap (53 degrees), below the picture.
	float ax = cx + SDL_cosf(53.0f * 3.14159265f / 180.0f) * R * 0.92f;
	float ay = cy + SDL_sinf(53.0f * 3.14159265f / 180.0f) * R * 0.92f;
	st_touch(SDL_EVENT_FINGER_DOWN, 6, ax, ay);
	fails += st_check("botao A segurado", st_frames(3), KEY_A);
	st_touch(SDL_EVENT_FINGER_UP, 6, ax, ay);
	st_frames(6);

	input_crown(2);
	uint16_t seq = 0;
	int presses = 0;
	for (int i = 0; i < 16; ++i) {
		uint16_t k = input_frame();
		if ((k & KEY_DOWN) && !(seq & KEY_DOWN)) ++presses;
		seq = k;
	}
	fails += st_check("coroa 2 cliques = 2x baixo", presses == 2 ? KEY_DOWN : 0, KEY_DOWN);

	SDL_Log("selftest: %s (%d falha(s))", fails ? "FALHOU" : "OK", fails);
	return fails ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
	if (A.selftest) {
		return selftest();
	}
	if (A.pattern) {
		return pattern_iterate();
	}
	if (A.running && A.testFrames) {
		return test_iterate();
	}
	if (A.running) {
		run_emulation();
		view_render(A.ren, A.tex);
	} else {
		SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255);
		SDL_RenderClear(A.ren);
		SDL_SetRenderDrawColor(A.ren, 255, 255, 255, 255);
		int w, h;
		SDL_GetRenderOutputSize(A.ren, &w, &h);
		float y = h * 0.35f;
		SDL_RenderDebugText(A.ren, 16, y, "Final Fantasy Tactics Advance");
		SDL_RenderDebugText(A.ren, 16, y + 20, "ROM nao encontrada. Copie para:");
		SDL_RenderDebugText(A.ren, 16, y + 36, A.base);
		SDL_RenderDebugText(A.ren, 16, y + 52, "com o nome rom.gba");
	}
	SDL_RenderPresent(A.ren);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
	if (A.running) {
		SDL_LockMutex(A.coreLock);
		emu_shutdown();
		A.running = false;
		SDL_UnlockMutex(A.coreLock);
	}
	input_shutdown();
	if (A.audio) {
		SDL_DestroyAudioStream(A.audio);
	}
}
