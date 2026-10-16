#include "port.h"

#include <stdio.h>
#include <string.h>

#ifdef __WII__
#include <gccore.h>
#include <ogc/ax.h>
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
static bool wii_port_audio_ready = false;
static uint32_t wii_port_width = 640;
static uint32_t wii_port_height = 480;

#ifdef __WII__
static void *wii_port_xfb[2] = { NULL, NULL };
static u32 wii_port_fbb = 0;
static GXRModeObj *wii_port_rmode = NULL;
#endif

void wii_port_init(void)
{
#ifdef __WII__
    if (wii_port_ready) {
        return;
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

void wii_port_shutdown(void)
{
#ifdef __WII__
    if (wii_port_audio_ready) {
        AX_Pause(0);
        wii_port_audio_ready = false;
    }
    WPAD_Shutdown();
    PAD_Shutdown();
    wii_port_ready = false;
    printf("[WII] platform shutdown\n");
#else
    (void)0;
#endif
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

    GX_Init();
    GX_SetViewport(0.0f, 0.0f, (float)wii_port_rmode->fbWidth, (float)wii_port_rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDispCopySrc(0, 0, wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetDispCopyDst(wii_port_rmode->fbWidth, wii_port_rmode->efbHeight);
    GX_SetCopyFilter(wii_port_rmode->aa, wii_port_rmode->sample_pattern, GX_TRUE, wii_port_rmode->vfilter);
    GX_SetFieldMode(wii_port_rmode->field_rendering, GX_ENABLE);

    if (config->audio_enabled) {
        AX_Init();
        wii_port_audio_ready = true;
    }

    if (config->ir_enabled) {
        WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_0);
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

    if (audio->channels > 0 && audio->bits_per_sample > 0) {
        wii_port_audio_ready = true;
    }

    /*
     * Native Wii path:
     *  - init AX if not already active
     *  - prepare a 16-bit stereo stream contract for the Time Crisis runtime
     *  - keep the rest of the game untouched by normalizing output through the
     *    backend API instead of SDL or desktop mixer code
     */
    if (wii_port_audio_ready) {
        AX_Init();
    }

    printf("[WII] setup AX audio %u Hz, %u ch, %u bits\n",
           audio->sample_rate,
           audio->channels,
           audio->bits_per_sample);
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

    if (WPADData[pad_chan].ir.valid) {
        input->gun_x = (uint16_t)WPADData[pad_chan].ir.x;
        input->gun_y = (uint16_t)WPADData[pad_chan].ir.y;
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
    GX_InvalidateTexAll();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetScissor(0, 0, wii_port_width, wii_port_height);
#endif
}

void wii_port_end_frame(const wii_platform_frame *frame)
{
#ifdef __WII__
    GX_DrawDone();
    GX_Flush();
    if (frame && frame->pixels) {
        GX_CopyDisp(frame->pixels, GX_FALSE);
    } else {
        GX_CopyDisp(NULL, GX_FALSE);
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
    wii_port_begin_frame();
    wii_port_end_frame(frame);
}

void wii_port_submit_audio(const int16_t *samples, size_t sample_count)
{
#ifdef __WII__
    if (!samples || sample_count == 0) {
        return;
    }

    if (!wii_port_audio_ready) {
        return;
    }

    /*
     * Real implementation path:
     *  - build an AX voice or stream
     *  - feed PCM data into the Wii audio backend
     *  - keep the sample count aligned to the selected format
     *
     * For now the backend keeps the game logic platform-neutral by accepting a
     * normalized 16-bit stereo stream and deferring final AX routing to the
     * native Wii audio stack.
     */
#else
    (void)samples;
    (void)sample_count;
#endif
}

bool wii_port_load_rom(const char *path, void *buffer, size_t size)
{
    (void)path;
    (void)buffer;
    (void)size;
    return false;
}

const char *wii_port_storage_root(void)
{
#ifdef __WII__
    return "/sd/";
#else
    return ".";
#endif
}
