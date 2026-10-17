#include <stdbool.h>
#include <string.h>

#include "ss22_input.h"
#include "ss22_board.h"
#include "ss22_host.h"
#include "../timecris/wii/port.h"

static const ss22_input_game *game;
static float aim_x = 0.5f, aim_y = 0.5f;
static bool aim_on;

const eng_ui_page *ss22_input_page(void)
{
    return NULL;
}

void ss22_input_init(const ss22_input_game *g)
{
    game = g;
}

void ss22_input_event(const SDL_Event *e)
{
    (void)e;
}

void ss22_input_update(void)
{
    if (!game) return;

    wii_platform_input in;
    wii_port_poll_input(&in);

    uint16_t pressed = 0;
    unsigned wheel = 0x200;
    unsigned pedal[2] = { 0, 0 };

    for (int i = 0; i < game->n; i++) {
        const ss22_action *ac = &game->actions[i];
        bool held = false;

        if (ac->bit == 0) {
            continue;
        }

        if (!strcmp(ac->key, "coin") && in.coin) held = true;
        else if (!strcmp(ac->key, "service") && in.service) held = true;
        else if (!strcmp(ac->key, "test") && in.test) held = true;
        else if (!strcmp(ac->key, "trigger") && in.trigger) held = true;
        else if (!strcmp(ac->key, "pedal") && in.pedal) held = true;

        if (held) pressed |= ac->bit;
    }

    if (game->light_gun) {
        /* Every frame, as the desktop mouse: the gun's position is live, and pointing off-screen is how the player reloads. */
        aim_on = !in.offscreen;
        if (aim_on) {
            uint32_t ir_w, ir_h;
            wii_port_ir_size(&ir_w, &ir_h);
            const float nx = (float)in.gun_x / (float)ir_w;
            const float ny = (float)in.gun_y / (float)ir_h;
            aim_x = nx < 0.0f ? 0.0f : nx > 1.0f ? 1.0f : nx;
            aim_y = ny < 0.0f ? 0.0f : ny > 1.0f ? 1.0f : ny;
            g_ss22_gun_x = (uint16_t)(68 + aim_x * 626);
            g_ss22_gun_y = (uint16_t)(43 + aim_y * 241);
        }
        g_ss22_gun_off = !aim_on;
    }

    if (game->service_bit && in.service) pressed |= game->service_bit;
    if (game->test_bit && in.test) pressed |= game->test_bit;

    game->send(pressed, wheel, pedal[0], pedal[1]);
}

void ss22_input_neutral(void)
{
    if (game) {
        game->send(game->test_bit ? game->test_bit : 0, 0x200, 0, 0);
    }
    aim_on = false;
    g_ss22_gun_off = true;
}

bool ss22_input_aim(float *nx, float *ny)
{
    if (!game || !game->light_gun || !aim_on) return false;
    if (nx) *nx = aim_x;
    if (ny) *ny = aim_y;
    return true;
}

void ss22_input_motor(uint8_t b)
{
    (void)b;
}

void ss22_input_rumble(uint16_t low, uint16_t high, uint32_t ms)
{
    (void)low; (void)high; (void)ms;
}

void ss22_input_kick(void)
{
}

void ss22_input_kick_frame(void)
{
}

void ss22_input_close(void)
{
}
