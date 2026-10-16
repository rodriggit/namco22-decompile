#ifndef ENG_WII_PORT_H
#define ENG_WII_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wii platform API for the Time Crisis port.
 *
 * The goal is to keep the game logic platform-agnostic: the game asks for
 * hardware services through this interface, and the Wii backend implements them
 * using devkitPro libraries (GX, AX, PAD/WPAD, SD access, etc.).
 */

typedef struct wii_platform_input {
    /*
     * Wii-native control layout:
     *  - trigger  -> Wiimote B button (mandatory)
     *  - coin     -> Wiimote + (or Home if using a one-button scheme)
     *  - service  -> Wiimote -
     *  - test     -> Wiimote Home
     *  - pedal    -> Nunchuk Z or Wiimote 1
     *  - offscreen -> pointer invalid / IR not detected
     *
     * The game receives normalized trigger/aim state and does not care which
     * physical controller produced it.
     */
    bool coin;
    bool service;
    bool test;
    bool trigger;
    bool pedal;
    bool offscreen;
    /*
     * Wii pointer coordinates are expressed in the host's native screen-space
     * convention so the rest of the game does not need to know about IR
     * resolution or Wii coordinate quirks. The backend keeps this translation
     * internal to the platform layer.
     */
    uint16_t gun_x;
    uint16_t gun_y;
    int16_t stick_x;
    int16_t stick_y;
} wii_platform_input;

enum wii_platform_frame_format {
    WII_FRAME_FORMAT_INVALID = 0,
    WII_FRAME_FORMAT_RGB565 = 1,
    WII_FRAME_FORMAT_XRGB8888 = 2,
};

typedef struct wii_platform_frame {
    /*
     * Wii-native frame contract:
     *  - width / height describe the logical frame size
     *  - pitch is the byte stride in the submitted pixel buffer
     *  - format identifies the native pixel representation.
     *
     * The host is expected to submit a framebuffer in a format compatible with
     * the GX backend; the game logic itself remains unaware of the hardware.
     */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    void *pixels;
} wii_platform_frame;

typedef struct wii_platform_config {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    bool ir_enabled;
    bool audio_enabled;
} wii_platform_config;

typedef struct wii_platform_audio {
    /*
     * Native Wii audio contract:
     *  - sample_rate: target AX output rate
     *  - channels: 1 or 2, typically stereo for the engine output
     *  - bits_per_sample: 16-bit PCM is the expected default
     */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} wii_platform_audio;

void wii_port_init(void);
void wii_port_shutdown(void);
bool wii_port_is_ready(void);
void wii_port_configure(const wii_platform_config *config);
void wii_port_setup_audio(const wii_platform_audio *audio);
void wii_port_poll_input(wii_platform_input *input);
void wii_port_begin_frame(void);
void wii_port_end_frame(const wii_platform_frame *frame);
void wii_port_submit_frame(const wii_platform_frame *frame);
void wii_port_submit_audio(const int16_t *samples, size_t sample_count);
bool wii_port_load_rom(const char *path, void *buffer, size_t size);
const char *wii_port_storage_root(void);

#endif
