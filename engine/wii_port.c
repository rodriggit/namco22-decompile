#include "wii_port.h"

#include <stdio.h>
#include <string.h>

/*
 * This file is intentionally a stubbed platform layer.
 *
 * The real Wii implementation will be provided when the project is built with
 * devkitPro, replacing these default no-op operations with GX/AX/PAD/WPAD/SD
 * access. The game logic stays unchanged and continues to call the same API.
 */

void wii_port_init(void)
{
#ifdef __WII__
    printf("[WII] platform init\n");
#else
    (void)0;
#endif
}

void wii_port_shutdown(void)
{
#ifdef __WII__
    printf("[WII] platform shutdown\n");
#else
    (void)0;
#endif
}

void wii_port_poll_input(wii_platform_input *input)
{
    if (!input) return;
    memset(input, 0, sizeof(*input));

    /*
     * Real Wii implementation:
     * - read PAD/WPAD state
     * - map button states to coin / service / test / trigger / pedal
     * - map analog or pointer input to gun_x / gun_y / offscreen
     */
}

void wii_port_begin_frame(void)
{
#ifdef __WII__
    /* Real implementation: prepare the GX frame / swapchain state. */
#endif
}

void wii_port_end_frame(const wii_platform_frame *frame)
{
    (void)frame;
#ifdef __WII__
    /* Real implementation: present the rendered output to the Wii display. */
#endif
}

void wii_port_submit_audio(const int16_t *samples, size_t sample_count)
{
    (void)samples;
    (void)sample_count;
#ifdef __WII__
    /* Real implementation: submit PCM to AX. */
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
