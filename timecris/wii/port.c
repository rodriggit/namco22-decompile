#include "port.h"

#include <stdio.h>
#include <string.h>

#ifdef __WII__
#include <gccore.h>
#include <asndlib.h>
#include <ogc/gx.h>
#include <ogc/pad.h>
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
 *  - pedal   => Nunchuk Z
 *
 * The rest of the game logic stays platform-neutral and receives a normalized
 * input state through wii_platform_input.
 */

static bool wii_port_ready = false;
static bool wii_port_video_ready = false;
static bool wii_port_audio_ready = false;
static bool wii_port_ax_started = false;
static uint32_t wii_port_width = 640;
static uint32_t wii_port_height = 480;
static uint32_t wii_port_audio_sample_rate = 32000;
static uint16_t wii_port_audio_channels = 2;
static uint16_t wii_port_audio_bits_per_sample = 16;

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
static void *wii_port_xfb[2] = { NULL, NULL };
static u32 wii_port_fbb = 0;
static GXRModeObj *wii_port_rmode = NULL;
static unsigned char wii_port_gx_fifo[256 * 1024] __attribute__((aligned(32)));
static int16_t wii_port_audio_ring[4096];
static size_t wii_port_audio_count = 0;
static size_t wii_port_audio_head = 0;
static size_t wii_port_audio_tail = 0;

static uint32_t wii_port_frame_pitch(uint32_t width)
{
    return width * sizeof(uint16_t);
}

static void wii_port_copy_frame_to_xfb(const wii_platform_frame *frame)
{
    if (!wii_port_xfb[wii_port_fbb] || !frame || !frame->pixels) {
        return;
    }

    uint16_t *dst = (uint16_t *)wii_port_xfb[wii_port_fbb];
    const uint32_t width = frame->width ? frame->width : wii_port_width;
    const uint32_t height = frame->height ? frame->height : wii_port_height;
    const uint32_t display_width = wii_port_rmode ? wii_port_rmode->fbWidth : width;
    const uint32_t dst_pitch = wii_port_frame_pitch(display_width);
    const uint32_t src_stride = frame->pitch ? frame->pitch : (width * sizeof(uint16_t));

    if (frame->format == WII_FRAME_FORMAT_XRGB8888) {
        const uint32_t *src = (const uint32_t *)frame->pixels;
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t *row = dst + (y * display_width);
            const uint32_t *src_row = src + (y * (src_stride / sizeof(uint32_t)));
            for (uint32_t x = 0; x < width && x < display_width; ++x) {
                const uint32_t value = src_row[x];
                const uint8_t r = (uint8_t)((value >> 16) & 0xFF);
                const uint8_t g = (uint8_t)((value >> 8) & 0xFF);
                const uint8_t b = (uint8_t)(value & 0xFF);
                row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    } else if (frame->format == WII_FRAME_FORMAT_RGB565) {
        const uint16_t *src = (const uint16_t *)frame->pixels;
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t *row = dst + (y * display_width);
            const uint16_t *src_row = src + (y * (src_stride / sizeof(uint16_t)));
            for (uint32_t x = 0; x < width && x < display_width; ++x) {
                row[x] = src_row[x];
            }
        }
    } else {
        memset(dst, 0, dst_pitch * height);
    }

    DCFlushRange(wii_port_xfb[wii_port_fbb], dst_pitch * height);
}
#endif

void wii_port_init(void)
{
#ifdef __WII__
    if (wii_port_ready) {
        return;
    }

    VIDEO_Init();
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

    GX_Init(wii_port_gx_fifo, (u32)sizeof(wii_port_gx_fifo));
    wii_port_video_ready = true;
    GX_SetViewport(0.0f, 0.0f, (float)wii_port_rmode->fbWidth, (float)wii_port_rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDispCopySrc(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetDispCopyDst(wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
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
    input->pedal = (wpad_buttons & WPAD_BUTTON_1) != 0 || (pad_buttons & PAD_BUTTON_B) != 0;

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
        memset(wii_port_xfb[wii_port_fbb], 0, wii_port_frame_pitch(wii_port_width) * wii_port_height);
        DCFlushRange(wii_port_xfb[wii_port_fbb], wii_port_frame_pitch(wii_port_width) * wii_port_height);
    }

    wii_port_fbb ^= 1;
    VIDEO_SetNextFramebuffer(wii_port_xfb[wii_port_fbb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
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

    if (!wii_port_video_ready || !wii_port_rmode || !frame) {
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
    } else {
        snprintf(candidate, sizeof(candidate), "/sd/%s", path);
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
    return "/sd/";
#else
    return ".";
#endif
}
