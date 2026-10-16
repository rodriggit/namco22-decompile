#include "port.h"

#include <stdio.h>
#include <string.h>

#ifdef __WII__
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

void wii_port_init(void)
{
#ifdef __WII__
    WPAD_Init();
    PAD_Init();
    printf("[WII] platform init\n");
#else
    (void)0;
#endif
}

void wii_port_shutdown(void)
{
#ifdef __WII__
    WPAD_Shutdown();
    PAD_Shutdown();
    printf("[WII] platform shutdown\n");
#else
    (void)0;
#endif
}

void wii_port_configure(const wii_platform_config *config)
{
#ifdef __WII__
    if (!config) return;
    /* Real implementation: initialize GX framebuffer and AX audio stream. */
    printf("[WII] configure display %ux%u @ %u Hz, ir=%s, audio=%s\n",
           config->width,
           config->height,
           config->refresh_hz,
           config->ir_enabled ? "on" : "off",
           config->audio_enabled ? "on" : "off");
#else
    (void)config;
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
    /* prepare GX frame */
#endif
}

void wii_port_end_frame(const wii_platform_frame *frame)
{
    (void)frame;
#ifdef __WII__
    /* present the frame on screen */
#endif
}

void wii_port_submit_audio(const int16_t *samples, size_t sample_count)
{
    (void)samples;
    (void)sample_count;
#ifdef __WII__
    /* stream audio to AX */
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
