#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __WII__
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <fat.h>
#include <ogc/gx.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <wiiuse/wpad.h>
#endif

/*
 * Wii backend implementation for the Time Crisis port.
 *
 * Native mapping used here:
 *  - trigger => Wiimote B
 *  - coin    => Wiimote +
 *  - service => Wiimote -
 *  - test    => Wiimote Home
 *  - pedal   => Nunchuk Z, Wiimote 1 or GameCube B
 *
 * The rest of the game logic stays platform-neutral and receives a normalized
 * input state through wii_platform_input.
 */

static bool wii_port_ready = false;
static bool wii_port_video_ready = false;
static bool wii_port_audio_ready = false;
static bool wii_port_ax_started = false;
static bool wii_port_fs_ready = false;
static uint32_t wii_port_width = 640;
static uint32_t wii_port_height = 480;
static uint32_t wii_port_ir_width = 640;     /* WPAD_SetVRes: the range of wii_platform_input.gun_x / gun_y */
static uint32_t wii_port_ir_height = 480;
static uint32_t wii_port_audio_sample_rate = 32000;
static uint16_t wii_port_audio_channels = 2;
static uint16_t wii_port_audio_bits_per_sample = 16;
static uint16_t *wii_port_status_pixels;
static size_t wii_port_status_pixels_cap;
static char wii_port_status_text[32];
static unsigned wii_port_status_frames;

static void wii_port_start_ax(void)
{
#ifdef __WII__
    if (wii_port_ax_started) {
        return;
    }

    ASND_Init();
    ASND_Pause(0);
    wii_port_ax_started = true;
    wii_port_audio_ready = true;
#endif
}

#ifdef __WII__
static void wii_port_fill_status_pixels(uint16_t *dst, uint32_t width, uint32_t height, const char *text)
{
    static const uint8_t glyph_a[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const uint8_t glyph_b[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const uint8_t glyph_g[7] = { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E };
    static const uint8_t glyph_d[7] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E };
    static const uint8_t glyph_e[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
    static const uint8_t glyph_h[7] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const uint8_t glyph_i[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F };
    static const uint8_t glyph_n[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
    static const uint8_t glyph_o[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    static const uint8_t glyph_r[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const uint8_t glyph_t[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const uint8_t glyph_v[7] = { 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 };
    static const uint8_t glyph_y[7] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
    const uint8_t *glyph = NULL;
    const uint16_t bg = 0x0000u;
    const uint16_t fg = 0xFFFFu;
    const int scale = width >= 640u ? 6 : 4;
    const int glyph_w = 5 * scale;
    const int glyph_gap = scale;
    size_t len = text ? strlen(text) : 0;
    int text_w = 0;

    for (size_t i = 0; i < len; ++i) {
        text_w += (text[i] == ' ') ? (glyph_w / 2) : (glyph_w + glyph_gap);
    }
    if (text_w > 0) text_w -= glyph_gap;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            dst[y * width + x] = bg;
        }
    }

    if (!text || !*text) {
        return;
    }

    int cursor = (int)((width - (uint32_t)text_w) / 2u);
    int baseline = (int)((height - 7 * scale) / 2u);

    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (c == ' ') {
            cursor += glyph_w / 2;
            continue;
        }
        switch (c) {
        case 'A': glyph = glyph_a; break;
        case 'B': glyph = glyph_b; break;
        case 'D': glyph = glyph_d; break;
        case 'E': glyph = glyph_e; break;
        case 'G': glyph = glyph_g; break;
        case 'H': glyph = glyph_h; break;
        case 'I': glyph = glyph_i; break;
        case 'N': glyph = glyph_n; break;
        case 'O': glyph = glyph_o; break;
        case 'R': glyph = glyph_r; break;
        case 'T': glyph = glyph_t; break;
        case 'V': glyph = glyph_v; break;
        case 'Y': glyph = glyph_y; break;
        default: glyph = NULL; break;
        }
        if (glyph) {
            for (int gy = 0; gy < 7; ++gy) {
                for (int gx = 0; gx < 5; ++gx) {
                    if (!(glyph[gy] & (uint8_t)(1u << (4 - gx)))) continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        uint16_t *row = dst + (baseline + gy * scale + sy) * width + cursor + gx * scale;
                        for (int sx = 0; sx < scale; ++sx) {
                            row[sx] = fg;
                        }
                    }
                }
            }
        }
        cursor += glyph_w + glyph_gap;
    }
}
#endif

#ifdef __WII__
static void *wii_port_xfb[2] = { NULL, NULL };
static u32 wii_port_fbb = 0;
static GXRModeObj *wii_port_rmode = NULL;
static unsigned char *wii_port_gx_fifo;
static const uint32_t wii_port_gx_fifo_size = 256u * 1024u;
static int16_t wii_port_audio_ring[4096];
static size_t wii_port_audio_count = 0;
static size_t wii_port_audio_head = 0;
static size_t wii_port_audio_tail = 0;

static uint32_t wii_port_frame_pitch(uint32_t width)
{
    return width * sizeof(uint16_t);
}

static inline uint8_t wii_clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline void wii_rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b,
                                    uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    /* ITU-R BT.601 integer approximation. */
    const int yi = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    const int cbi = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    const int cri = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    *y = wii_clamp_u8(yi);
    *cb = wii_clamp_u8(cbi);
    *cr = wii_clamp_u8(cri);
}

static void wii_port_copy_frame_to_xfb(const wii_platform_frame *frame)
{
    if (!wii_port_xfb[wii_port_fbb] || !frame || !frame->pixels) {
        return;
    }

    uint32_t *dst = (uint32_t *)wii_port_xfb[wii_port_fbb];
    const uint32_t width = frame->width ? frame->width : wii_port_width;
    const uint32_t src_height = frame->height ? frame->height : wii_port_height;
    const uint32_t height = (wii_port_rmode && src_height > wii_port_rmode->xfbHeight) ?
        wii_port_rmode->xfbHeight : src_height;
    const uint32_t display_width = wii_port_rmode ? wii_port_rmode->fbWidth : width;
    const uint32_t copy_w = width < display_width ? width : display_width;
    const uint32_t even_copy_w = copy_w & ~1u;
    const uint32_t dst_pitch = wii_port_frame_pitch(display_width);
    const uint32_t src_stride = frame->pitch ? frame->pitch :
        (frame->format == WII_FRAME_FORMAT_XRGB8888 ? width * sizeof(uint32_t) : width * sizeof(uint16_t));

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *dst_row = dst + y * (display_width / 2u);

        if (frame->format == WII_FRAME_FORMAT_XRGB8888) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)frame->pixels + y * src_stride);
            uint32_t x = 0;
            for (; x < even_copy_w; x += 2) {
                const uint32_t p0 = src[x + 0];
                const uint32_t p1 = src[x + 1];
                const uint8_t r0 = (uint8_t)((p0 >> 16) & 0xFF);
                const uint8_t g0 = (uint8_t)((p0 >> 8) & 0xFF);
                const uint8_t b0 = (uint8_t)(p0 & 0xFF);
                const uint8_t r1 = (uint8_t)((p1 >> 16) & 0xFF);
                const uint8_t g1 = (uint8_t)((p1 >> 8) & 0xFF);
                const uint8_t b1 = (uint8_t)(p1 & 0xFF);

                uint8_t y0, cb0, cr0, y1, cb1, cr1;
                wii_rgb_to_ycbcr(r0, g0, b0, &y0, &cb0, &cr0);
                wii_rgb_to_ycbcr(r1, g1, b1, &y1, &cb1, &cr1);
                const uint8_t cb = (uint8_t)(((unsigned)cb0 + (unsigned)cb1) / 2u);
                const uint8_t cr = (uint8_t)(((unsigned)cr0 + (unsigned)cr1) / 2u);
                dst_row[x / 2u] = ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) | ((uint32_t)y1 << 8) | (uint32_t)cr;
            }
            if (copy_w & 1u) {
                const uint32_t p = src[copy_w - 1u];
                const uint8_t r = (uint8_t)((p >> 16) & 0xFF);
                const uint8_t g = (uint8_t)((p >> 8) & 0xFF);
                const uint8_t b = (uint8_t)(p & 0xFF);
                uint8_t yy, cb, cr;
                wii_rgb_to_ycbcr(r, g, b, &yy, &cb, &cr);
                dst_row[copy_w / 2u] = ((uint32_t)yy << 24) | ((uint32_t)cb << 16) | ((uint32_t)yy << 8) | (uint32_t)cr;
            }
        } else if (frame->format == WII_FRAME_FORMAT_RGB565) {
            const uint16_t *src = (const uint16_t *)((const uint8_t *)frame->pixels + y * src_stride);
            uint32_t x = 0;
            for (; x < even_copy_w; x += 2) {
                const uint16_t p0 = src[x + 0];
                const uint16_t p1 = src[x + 1];
                const uint8_t r0 = (uint8_t)(((p0 >> 11) & 0x1F) * 255 / 31);
                const uint8_t g0 = (uint8_t)(((p0 >> 5) & 0x3F) * 255 / 63);
                const uint8_t b0 = (uint8_t)((p0 & 0x1F) * 255 / 31);
                const uint8_t r1 = (uint8_t)(((p1 >> 11) & 0x1F) * 255 / 31);
                const uint8_t g1 = (uint8_t)(((p1 >> 5) & 0x3F) * 255 / 63);
                const uint8_t b1 = (uint8_t)((p1 & 0x1F) * 255 / 31);

                uint8_t y0, cb0, cr0, y1, cb1, cr1;
                wii_rgb_to_ycbcr(r0, g0, b0, &y0, &cb0, &cr0);
                wii_rgb_to_ycbcr(r1, g1, b1, &y1, &cb1, &cr1);
                const uint8_t cb = (uint8_t)(((unsigned)cb0 + (unsigned)cb1) / 2u);
                const uint8_t cr = (uint8_t)(((unsigned)cr0 + (unsigned)cr1) / 2u);
                dst_row[x / 2u] = ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) | ((uint32_t)y1 << 8) | (uint32_t)cr;
            }
            if (copy_w & 1u) {
                const uint16_t p = src[copy_w - 1u];
                const uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                const uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
                const uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
                uint8_t yy, cb, cr;
                wii_rgb_to_ycbcr(r, g, b, &yy, &cb, &cr);
                dst_row[copy_w / 2u] = ((uint32_t)yy << 24) | ((uint32_t)cb << 16) | ((uint32_t)yy << 8) | (uint32_t)cr;
            }
        } else {
            for (uint32_t x = 0; x < display_width / 2u; ++x) {
                dst_row[x] = 0x10801080u;
            }
        }

        for (uint32_t x = (copy_w + 1u) / 2u; x < display_width / 2u; ++x) {
            dst_row[x] = 0x10801080u;
        }
    }
    DCFlushRange(wii_port_xfb[wii_port_fbb], dst_pitch * height);
}

static void wii_port_clear_xfb_black(void)
{
    if (!wii_port_xfb[wii_port_fbb]) {
        return;
    }

    const size_t words = (size_t)wii_port_frame_pitch(wii_port_width) * wii_port_height / sizeof(uint32_t);
    uint32_t *dst = (uint32_t *)wii_port_xfb[wii_port_fbb];
    for (size_t i = 0; i < words; ++i) {
        dst[i] = 0x10801080u;
    }
    DCFlushRange(wii_port_xfb[wii_port_fbb], words * sizeof(uint32_t));
}
#endif

void wii_port_init(void)
{
#ifdef __WII__
    if (wii_port_ready) {
        return;
    }

    VIDEO_Init();
    if (!wii_port_fs_ready) {
        wii_port_fs_ready = fatInitDefault();
        if (!wii_port_fs_ready) {
            fprintf(stderr, "[WII] fatInitDefault failed; ROM access on sd:/ may fail\n");
        }
    }
    WPAD_Init();
    PAD_Init();
    WPAD_SetVRes(0, 640, 480);
    wii_port_width = 640;
    wii_port_height = 480;
    wii_port_ready = true;
    printf("[WII] platform init\n");
#else
    (void)0;
#endif
}

#ifdef __WII__
static void wii_port_present_status_frame(const char *text)
{
    const uint32_t width = wii_port_width ? wii_port_width : 640u;
    const uint32_t height = wii_port_height ? wii_port_height : 480u;
    const size_t need = (size_t)width * (size_t)height;
    wii_platform_frame status_frame;

    if (need > wii_port_status_pixels_cap) {
        uint16_t *pixels = (uint16_t *)realloc(wii_port_status_pixels, need * sizeof(uint16_t));
        if (!pixels) {
            return;
        }
        wii_port_status_pixels = pixels;
        wii_port_status_pixels_cap = need;
    }

    wii_port_fill_status_pixels(wii_port_status_pixels, width, height, text);
    status_frame.width = width;
    status_frame.height = height;
    status_frame.pitch = width * (uint32_t)sizeof(uint16_t);
    status_frame.format = WII_FRAME_FORMAT_RGB565;
    status_frame.pixels = wii_port_status_pixels;
    wii_port_begin_frame();
    wii_port_end_frame(&status_frame);
}
#endif

void wii_port_shutdown(void)
{
#ifdef __WII__
    if (wii_port_audio_ready) {
        ASND_Pause(1);
        ASND_End();
        wii_port_audio_ready = false;
    }
    if (wii_port_video_ready) {
        GX_AbortFrame();
        wii_port_video_ready = false;
    }
    wii_port_ax_started = false;
    wii_port_audio_count = 0;
    wii_port_audio_head = 0;
    wii_port_audio_tail = 0;
    wii_port_xfb[0] = NULL;
    wii_port_xfb[1] = NULL;
    wii_port_rmode = NULL;
    WPAD_Shutdown();
    wii_port_ready = false;
    printf("[WII] platform shutdown\n");
#else
    (void)0;
#endif
}

bool wii_port_is_ready(void)
{
    return wii_port_ready;
}

void wii_port_configure(const wii_platform_config *config)
{
#ifdef __WII__
    if (!config) return;

    if (!wii_port_ready) {
        wii_port_init();
    }

    wii_port_width = config->width ? config->width : 640;
    wii_port_height = config->height ? config->height : 480;

    wii_port_rmode = VIDEO_GetPreferredMode(NULL);
    if (wii_port_rmode) {
        wii_port_width = (uint32_t)wii_port_rmode->fbWidth;
        wii_port_height = (uint32_t)wii_port_rmode->efbHeight;
    }
    if (!wii_port_rmode) {
        return;
    }

    VIDEO_Configure(wii_port_rmode);
    wii_port_xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(wii_port_rmode));
    wii_port_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(wii_port_rmode));
    wii_port_fbb = 0;
    VIDEO_SetNextFramebuffer(wii_port_xfb[wii_port_fbb]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (wii_port_rmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }

    if (!wii_port_gx_fifo) {
        wii_port_gx_fifo = (unsigned char *)wii_port_mem2_alloc(wii_port_gx_fifo_size, 32);
        if (!wii_port_gx_fifo) {
            fprintf(stderr, "[WII] GX FIFO allocation failed (%u bytes)\n", (unsigned)wii_port_gx_fifo_size);
            return;
        }
    }

    GX_Init(wii_port_gx_fifo, (u32)wii_port_gx_fifo_size);
    wii_port_video_ready = true;
    GX_SetViewport(0.0f, 0.0f, (float)wii_port_rmode->fbWidth, (float)wii_port_rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDispCopySrc(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    {
        /* the EFB is efbHeight lines and the XFB xfbHeight (equal on NTSC, 528 -> 574 on PAL): the copy scales between them */
        const f32 yscale = GX_GetYScaleFactor(wii_port_rmode->efbHeight, wii_port_rmode->xfbHeight);
        const u32 xfb_lines = GX_SetDispCopyYScale(yscale);
        GX_SetDispCopyDst(wii_port_rmode->fbWidth, xfb_lines);
    }
    GX_SetCopyFilter(wii_port_rmode->aa, wii_port_rmode->sample_pattern, GX_TRUE, wii_port_rmode->vfilter);
    GX_SetFieldMode(wii_port_rmode->field_rendering, GX_ENABLE);
    GX_SetCopyClear((GXColor){0, 0, 0, 0xFF}, GX_MAX_Z24);

    if (config->audio_enabled) {
        wii_port_start_ax();
        wii_port_audio_ready = true;
    }

    if (config->ir_enabled) {
        WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
        WPAD_SetVRes(WPAD_CHAN_0, wii_port_width, wii_port_height);
        wii_port_ir_width = wii_port_width;
        wii_port_ir_height = wii_port_height;
    }

    printf("[WII] configure display %ux%u @ %u Hz, ir=%s, audio=%s\n",
           wii_port_width,
           wii_port_height,
           config->refresh_hz,
           config->ir_enabled ? "on" : "off",
           config->audio_enabled ? "on" : "off");
#else
    (void)config;
#endif
}

void wii_port_setup_audio(const wii_platform_audio *audio)
{
#ifdef __WII__
    if (!audio) return;

    if (!wii_port_ready) {
        wii_port_init();
    }

    if (audio->sample_rate > 0) {
        wii_port_audio_sample_rate = audio->sample_rate;
    }
    if (audio->channels > 0) {
        wii_port_audio_channels = audio->channels;
    }
    if (audio->bits_per_sample > 0) {
        wii_port_audio_bits_per_sample = audio->bits_per_sample;
    }

    if (wii_port_audio_channels > 0 && wii_port_audio_bits_per_sample > 0) {
        wii_port_audio_ready = true;
    }

    if (wii_port_audio_ready) {
        wii_port_start_ax();
    }

    printf("[WII] setup AX audio %u Hz, %u ch, %u bits\n",
           wii_port_audio_sample_rate,
           wii_port_audio_channels,
           wii_port_audio_bits_per_sample);
#else
    (void)audio;
#endif
}

void wii_port_poll_input(wii_platform_input *input)
{
    if (!input) return;
    memset(input, 0, sizeof(*input));

#ifdef __WII__
    WPAD_ScanPads();
    PAD_ScanPads();

    const u32 wpad_buttons = WPAD_ButtonsHeld(0);
    const u32 pad_buttons = PAD_ButtonsHeld(0);
    const int pad_chan = 0;

    /* Mandatory mapping: trigger is the Wiimote B button. */
    input->trigger = (wpad_buttons & WPAD_BUTTON_B) != 0;

    /* Practical defaults for the remaining controls. */
    input->coin = (wpad_buttons & WPAD_BUTTON_PLUS) != 0;
    input->service = (wpad_buttons & WPAD_BUTTON_MINUS) != 0;
    input->test = (wpad_buttons & WPAD_BUTTON_HOME) != 0;
    input->pedal = (wpad_buttons & (WPAD_BUTTON_1 | WPAD_NUNCHUK_BUTTON_Z)) != 0 || (pad_buttons & PAD_BUTTON_B) != 0;

    WPADData *wd = WPAD_Data(pad_chan);
    if (wd && wd->ir.valid) {
        input->gun_x = (uint16_t)wd->ir.x;
        input->gun_y = (uint16_t)wd->ir.y;
        input->offscreen = false;
    } else {
        input->offscreen = true;
    }
#else
    (void)0;
#endif
}

void wii_port_ir_size(uint32_t *w, uint32_t *h)
{
    /* wii_port_configure passes these to WPAD_SetVRes. */
    if (w) *w = wii_port_ir_width ? wii_port_ir_width : 640u;
    if (h) *h = wii_port_ir_height ? wii_port_ir_height : 480u;
}

void wii_port_begin_frame(void)
{
#ifdef __WII__
    if (!wii_port_ready || !wii_port_video_ready || !wii_port_rmode) {
        return;
    }

    GX_InvalidateTexAll();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetScissor(0, 0, wii_port_width, wii_port_height);
    GX_SetViewport(0.0f, 0.0f, (float)wii_port_width, (float)wii_port_height, 0.0f, 1.0f);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
#endif
}

void wii_port_end_frame(const wii_platform_frame *frame)
{
#ifdef __WII__
    if (!wii_port_ready || !wii_port_video_ready || !wii_port_rmode) {
        return;
    }

    GX_DrawDone();
    GX_Flush();

    if (frame && frame->pixels) {
        wii_port_copy_frame_to_xfb(frame);
    } else {
        wii_port_clear_xfb_black();
    }

    VIDEO_SetNextFramebuffer(wii_port_xfb[wii_port_fbb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    wii_port_fbb ^= 1;
#else
    (void)frame;
#endif
}

void wii_port_submit_frame(const wii_platform_frame *frame)
{
#ifdef __WII__
    if (!wii_port_ready) {
        wii_port_init();
    }

    if (!wii_port_video_ready || !wii_port_rmode) {
        return;
    }

    if (wii_port_status_frames > 0 && wii_port_status_text[0]) {
        if (wii_port_status_frames > 0) {
            wii_port_status_frames--;
        }
    }

    if (!frame) {
        return;
    }

    if (frame->width > 0) {
        wii_port_width = frame->width;
    }
    if (frame->height > 0) {
        wii_port_height = frame->height;
    }
#endif

    wii_port_begin_frame();
    wii_port_end_frame(frame);
}

void wii_port_show_status(const char *text)
{
#ifdef __WII__
    if (!wii_port_ready) {
        wii_port_init();
    }
    if (!wii_port_video_ready || !wii_port_rmode) {
        wii_platform_config cfg = { 640u, 480u, 60u, false, false };
        wii_port_configure(&cfg);
    }
    if (!wii_port_video_ready || !wii_port_rmode) {
        return;
    }

    if (!text) {
        text = "";
    }
    snprintf(wii_port_status_text, sizeof(wii_port_status_text), "%s", text);
    wii_port_status_frames = 1u;
    wii_port_present_status_frame(wii_port_status_text);
#else
    (void)text;
#endif
}

void wii_port_submit_audio(const int16_t *samples, size_t sample_count)
{
#ifdef __WII__
    if (!samples || sample_count == 0) {
        return;
    }

    if (!wii_port_ready || !wii_port_audio_ready) {
        return;
    }

    wii_port_start_ax();

    const size_t limit = sizeof(wii_port_audio_ring) / sizeof(wii_port_audio_ring[0]);
    size_t copied = 0;
    while (copied < sample_count) {
        const int16_t v = samples[copied++];
        wii_port_audio_ring[wii_port_audio_head] = v;
        wii_port_audio_head = (wii_port_audio_head + 1u) % limit;
        if (wii_port_audio_count < limit) {
            wii_port_audio_count++;
        } else {
            wii_port_audio_tail = (wii_port_audio_tail + 1u) % limit;
        }
    }

    ASND_Pause(0);
#else
    (void)samples;
    (void)sample_count;
#endif
}

void *wii_port_mem2_alloc(size_t size, size_t align)
{
#ifdef __WII__
    if (align == 0) align = 32;
    if (align & (align - 1)) return NULL;

    uintptr_t lo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
    uintptr_t p = (lo + (uintptr_t)(align - 1)) & ~(uintptr_t)(align - 1);
    uintptr_t end = p + size;
    if (end < p || end > hi) return NULL;

    SYS_SetArena2Lo((void *)end);
    return (void *)p;
#else
    (void)size;
    (void)align;
    return NULL;
#endif
}

bool wii_port_gx_ready(void)
{
#ifdef __WII__
    return wii_port_ready && wii_port_video_ready && wii_port_rmode && wii_port_xfb[0] && wii_port_xfb[1];
#else
    return false;
#endif
}

void wii_port_gx_present(void)
{
#ifdef __WII__
    if (!wii_port_gx_ready()) {
        return;
    }
    /* The frame is in the EFB: copy it to the XFB the VI is not showing (clearing the EFB on the way), let the GPU finish,
     * and show it from the next field. That XFB is still on screen until the flip asked for last frame has happened (the VI
     * latches it at a retrace): only then is there anything to wait for. A frame slower than a field (every frame, today)
     * waits for nothing; a faster one still waits, so the game is never shown faster than the display. */
    static bool flip_pending;
    if (flip_pending && VIDEO_GetCurrentFramebuffer() == wii_port_xfb[wii_port_fbb]) {
        VIDEO_WaitVSync();
    }
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(wii_port_xfb[wii_port_fbb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(wii_port_xfb[wii_port_fbb]);
    VIDEO_Flush();
    flip_pending = true;
    wii_port_fbb ^= 1;
#endif
}

void wii_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    FILE *fp = fopen("wii_boot.log", "a");
    if (fp) {
        vfprintf(fp, fmt, ap);
        fputc('\n', fp);
        fclose(fp);
    }
    va_end(ap);
}

uint64_t wii_port_usec(void)
{
#ifdef __WII__
    return ticks_to_microsecs(gettime());
#else
    return 0;
#endif
}

void wii_port_mem_report(char *buf, size_t n)
{
#ifdef __WII__
    const size_t mem1 = (size_t)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
    const size_t mem2 = (size_t)((uintptr_t)SYS_GetArena2Hi() - (uintptr_t)SYS_GetArena2Lo());
    size_t lo = 0, hi = 64u << 20;

    /* Binary search for the largest single malloc that still succeeds (1 KB resolution). The pointer is volatile: GCC may
     * otherwise drop an unused malloc/free pair and assume it succeeded (it reported 64 MB with 11 MB free). */
    while (hi - lo > 1024u) {
        const size_t mid = lo + (hi - lo) / 2u;
        void *volatile p = malloc(mid);
        if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    snprintf(buf, n, "arena1 free %u KB, arena2 free %u KB, largest malloc %u KB",
             (unsigned)(mem1 >> 10), (unsigned)(mem2 >> 10), (unsigned)(lo >> 10));
#else
    snprintf(buf, n, "n/a");
#endif
}

bool wii_port_load_rom(const char *path, void *buffer, size_t size)
{
#ifdef __WII__
    char candidate[512];
    FILE *fp;
    size_t read_size;

    if (!path || !buffer || size == 0) {
        return false;
    }

    if (path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", path);
    } else if (strncmp(path, "sd:/", 4) == 0) {
        snprintf(candidate, sizeof(candidate), "%s", path);
    } else {
        snprintf(candidate, sizeof(candidate), "sd:/%s", path);
    }

    fp = fopen(candidate, "rb");
    if (!fp) {
        return false;
    }

    read_size = fread(buffer, 1, size, fp);
    fclose(fp);
    return read_size == size;
#else
    (void)path;
    (void)buffer;
    (void)size;
    return false;
#endif
}

const char *wii_port_storage_root(void)
{
#ifdef __WII__
    return "sd:/";
#else
    return ".";
#endif
}
