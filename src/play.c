/* play.c - interactive SDL2 frontend (the playable front-end).
 *
 * Runs the emulator in real time, draws the 160x144 framebuffer to a scaled
 * window with the classic DMG green palette, and maps the keyboard to the
 * joypad. F5/F9 quick-save/load a state. A headless smoke mode (--frames N,
 * works under SDL_VIDEODRIVER=dummy) drives the same loop for verification.
 *
 *   Controls: arrows = D-pad · Z = A · X = B · Enter = Start · Shift = Select
 *             F5 = save state (quick.gss) · F9 = load · Esc = quit
 */
#include "gb.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GB gb;

/* Classic DMG green shades, indexed by framebuffer value 0 (lightest)..3. */
static const u32 PALETTE[4] = {0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F};

#define LCD_W 160
#define LCD_H 144
#define FRAME_MS (1000.0 / 59.727)   /* DMG refresh */

static u8 read_keys(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u8 b = 0;
    if (k[SDL_SCANCODE_RIGHT]) b |= 0x01;
    if (k[SDL_SCANCODE_LEFT])  b |= 0x02;
    if (k[SDL_SCANCODE_UP])    b |= 0x04;
    if (k[SDL_SCANCODE_DOWN])  b |= 0x08;
    if (k[SDL_SCANCODE_Z])     b |= 0x10;   /* A */
    if (k[SDL_SCANCODE_X])     b |= 0x20;   /* B */
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) b |= 0x40;  /* Select */
    if (k[SDL_SCANCODE_RETURN]) b |= 0x80;  /* Start */
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.gb> [--scale N] [--frames N] [--png p]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1], *png = NULL;
    int scale = 4, limit = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
    }
    if (scale < 1) scale = 1;

    memset(&gb, 0, sizeof gb);
    if (cart_load(&gb, path) != 0) return 2;
    cpu_init_postboot(&gb);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* Audio: push synthesized APU samples each frame (best-effort). */
    SDL_AudioSpec want = {0};
    want.freq = APU_SAMPLE_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
    else fprintf(stderr, "[no audio: %s]\n", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow(gb.cart.title[0] ? gb.cart.title : "gbemu",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       LCD_W * scale, LCD_H * scale, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    /* The renderer may be unavailable under a headless/dummy video driver; in that
     * case we still run the emulation loop (and can dump frames), just without
     * presenting to a window. A real display gets the accelerated path. */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, LCD_W, LCD_H) : NULL;
    if (!ren) fprintf(stderr, "[headless: no renderer (%s) — running without display]\n",
                      SDL_GetError());
    u32 *pix = malloc(LCD_W * LCD_H * sizeof(u32));

    bool running = true;
    int rendered = 0;
    double next = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_F5:
                        gb_save_state(&gb, "quick.gss");
                        fprintf(stderr, "[state saved]\n");
                        break;
                    case SDLK_F9:
                        if (gb_load_state(&gb, "quick.gss") == 0)
                            fprintf(stderr, "[state loaded]\n");
                        break;
                    default: break;
                }
            }
        }
        gb.buttons = read_keys();

        /* run exactly one frame */
        u64 f = gb.frame_count;
        while (gb.frame_count == f) cpu_step(&gb);
        rendered++;

        /* push this frame's audio, dropping if the queue is backing up (>~0.2s) */
        if (adev) {
            i16 sbuf[2048];
            int n = apu_drain_samples(&gb, sbuf, 1024);
            if (n && SDL_GetQueuedAudioSize(adev) < APU_SAMPLE_RATE * 2 * sizeof(i16) / 5)
                SDL_QueueAudio(adev, sbuf, (Uint32)n * 2 * sizeof(i16));
        }

        if (tex) {
            for (int i = 0; i < LCD_W * LCD_H; i++) pix[i] = PALETTE[gb.fb[i] & 3];
            SDL_UpdateTexture(tex, NULL, pix, LCD_W * sizeof(u32));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        if (limit && rendered >= limit) {
            if (png) png_write_gray(png, LCD_W, LCD_H, gb.fb);
            running = false;
            break;
        }
        /* pace to ~59.7 fps without busy-waiting */
        next += FRAME_MS;
        double now = SDL_GetTicks();
        if (next > now) SDL_Delay((u32)(next - now));
        else next = now;
    }

    free(pix);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    cart_free(&gb);
    return 0;
}
