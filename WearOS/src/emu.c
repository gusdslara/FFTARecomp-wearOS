#include "emu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

#include <SDL3/SDL.h>

#include <mgba/gba/interface.h>

static struct mCore* s_core;
static uint32_t* s_fb;
static struct VFile* s_saveVf;

static void quiet_log(struct mLogger* l, int cat, enum mLogLevel level, const char* fmt, va_list args) {
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		char buf[512];
		vsnprintf(buf, sizeof(buf), fmt, args);
		SDL_Log("[mgba] %s", buf);
	}
}
static struct mLogger s_logger = { .log = quiet_log };

bool emu_init(const char* romPath, const char* savePath, char* err, int errLen) {
	mLogSetDefaultLogger(&s_logger);

	struct VFile* rom = VFileOpen(romPath, O_RDONLY);
	if (!rom) {
		snprintf(err, errLen, "ROM nao encontrada:\n%s", romPath);
		return false;
	}
	s_core = mCoreFindVF(rom);
	if (!s_core) {
		rom->close(rom);
		snprintf(err, errLen, "Arquivo nao e uma ROM de GBA valida");
		return false;
	}
	mCoreInitConfig(s_core, NULL);
	s_core->init(s_core);

	struct mCoreOptions opts = {
		.useBios = false,
		.skipBios = true,
		.volume = 0x100,
		.sampleRate = EMU_AUDIO_RATE,
	};
	mCoreConfigLoadDefaults(&s_core->config, &opts);
	mCoreConfigSetDefaultValue(&s_core->config, "idleOptimization", "detect");
	mCoreLoadConfig(s_core);

	s_fb = calloc(EMU_W * EMU_H, sizeof(uint32_t));
	s_core->setVideoBuffer(s_core, (color_t*) s_fb, EMU_W);

	// ~1 frame of audio per runFrame at 48 kHz is ~804 samples; give headroom.
	s_core->setAudioBufferSize(s_core, 2048);
	blip_set_rates(s_core->getAudioChannel(s_core, 0), s_core->frequency(s_core), EMU_AUDIO_RATE);
	blip_set_rates(s_core->getAudioChannel(s_core, 1), s_core->frequency(s_core), EMU_AUDIO_RATE);

	if (!s_core->loadROM(s_core, rom)) {
		snprintf(err, errLen, "Falha ao carregar ROM");
		return false;
	}

	if (savePath) {
		s_saveVf = VFileOpen(savePath, O_CREAT | O_RDWR);
		if (s_saveVf && !s_core->loadSave(s_core, s_saveVf)) {
			s_saveVf->close(s_saveVf);
			s_saveVf = NULL;
		}
		if (!s_saveVf) {
			SDL_Log("Aviso: save nao pode ser aberto em %s", savePath);
		}
	}

	s_core->reset(s_core);
	return true;
}

void emu_shutdown(void) {
	if (!s_core) {
		return;
	}
	// Destroying the core flushes and closes the savedata VFile.
	s_core->deinit(s_core);
	s_core = NULL;
	s_saveVf = NULL;
	free(s_fb);
	s_fb = NULL;
}

void emu_reset(void) {
	if (s_core) {
		s_core->reset(s_core);
	}
}

void emu_run_frame(uint16_t keys) {
	s_core->setKeys(s_core, keys);
	s_core->runFrame(s_core);
}

const uint32_t* emu_framebuffer(void) {
	return s_fb;
}

int emu_read_audio(int16_t* out, int maxFrames) {
	blip_t* l = s_core->getAudioChannel(s_core, 0);
	blip_t* r = s_core->getAudioChannel(s_core, 1);
	int avail = blip_samples_avail(l);
	if (avail > maxFrames) {
		avail = maxFrames;
	}
	blip_read_samples(l, out, avail, true);
	blip_read_samples(r, out + 1, avail, true);
	return avail;
}

double emu_fps(void) {
	return (double) s_core->frequency(s_core) / (double) s_core->frameCycles(s_core);
}

bool emu_save_state(const char* path) {
	size_t size = s_core->stateSize(s_core);
	void* buf = malloc(size);
	bool ok = s_core->saveState(s_core, buf) && SDL_SaveFile(path, buf, size);
	free(buf);
	return ok;
}

bool emu_load_state(const char* path) {
	size_t size = 0;
	void* buf = SDL_LoadFile(path, &size);
	if (!buf) {
		return false;
	}
	bool ok = size == s_core->stateSize(s_core) && s_core->loadState(s_core, buf);
	SDL_free(buf);
	return ok;
}

uint16_t emu_dispcnt(void) {
	return (uint16_t) s_core->busRead16(s_core, 0x04000000);
}


void emu_rom_id(char* title, char* code) {
	title[0] = code[0] = 0;
	if (!s_core) {
		return;
	}
	for (int i = 0; i < 12; ++i) {
		title[i] = (char) s_core->busRead8(s_core, 0x080000A0 + i);
	}
	title[12] = 0;
	for (int i = 0; i < 4; ++i) {
		code[i] = (char) s_core->busRead8(s_core, 0x080000AC + i);
	}
	code[4] = 0;
}
