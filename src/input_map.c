/*
 * Key and controller bindings for the Xbox buttons (input_map.h). The
 * defaults are the mapping the port started with.
 */

#include "input_map.h"

#include <stdio.h>
#include <string.h>

static const struct {
    const char  *name, *ini;    /* shown; in the settings file */
    mdr_binding  def;
} buttons[MDR_IN_COUNT] = {
    [MDR_IN_A]      = { "A",       "a",      { SDL_SCANCODE_SPACE,  SDL_GAMEPAD_BUTTON_SOUTH, SDL_BUTTON_LEFT } },
    [MDR_IN_B]      = { "B",       "b",      { SDL_SCANCODE_LSHIFT, SDL_GAMEPAD_BUTTON_EAST } },
    [MDR_IN_X]      = { "X",       "x",      { SDL_SCANCODE_F,      SDL_GAMEPAD_BUTTON_WEST } },
    [MDR_IN_Y]      = { "Y",       "y",      { SDL_SCANCODE_R,      SDL_GAMEPAD_BUTTON_NORTH } },
    [MDR_IN_BLACK]  = { "Black",   "black",  { SDL_SCANCODE_C,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER } },
    [MDR_IN_WHITE]  = { "White",   "white",  { SDL_SCANCODE_Z,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER } },
    [MDR_IN_LTRIG]  = { "L trig",  "lt",     { SDL_SCANCODE_Q,      MDR_PAD_AXIS + SDL_GAMEPAD_AXIS_LEFT_TRIGGER } },
    [MDR_IN_RTRIG]  = { "R trig",  "rt",     { SDL_SCANCODE_E,      MDR_PAD_AXIS + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER } },
    [MDR_IN_START]  = { "Start",   "start",  { SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START } },
    [MDR_IN_BACK]   = { "Back",    "back",   { SDL_SCANCODE_TAB,    SDL_GAMEPAD_BUTTON_BACK } },
    [MDR_IN_UP]     = { "D-pad up","up",     { SDL_SCANCODE_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP } },
    [MDR_IN_DOWN]   = { "D-pad dn","down",   { SDL_SCANCODE_DOWN,   SDL_GAMEPAD_BUTTON_DPAD_DOWN } },
    [MDR_IN_LEFT]   = { "D-pad lt","left",   { SDL_SCANCODE_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_LEFT } },
    [MDR_IN_RIGHT]  = { "D-pad rt","right",  { SDL_SCANCODE_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT } },
    [MDR_IN_LTHUMB] = { "L stick", "lthumb", { SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_LEFT_STICK } },
    [MDR_IN_RTHUMB] = { "R stick", "rthumb", { SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_RIGHT_STICK } },
};

static mdr_binding bindings[MDR_IN_COUNT];
static SDL_SpinLock mouse_lock;
static float mouse_dx, mouse_dy;
static SDL_AtomicInt mouse_held;

void mdr_mouse_set_captured(bool on)
{
    /* 2: just taken -- the buttons count once they have all been let go
     * (the click that took the mouse is not a press in the game). */
    if (!on)
        SDL_SetAtomicInt(&mouse_held, 0);
    else
        SDL_CompareAndSwapAtomicInt(&mouse_held, 0, 2);
}

SDL_MouseButtonFlags mdr_mouse_buttons(void)
{
    SDL_MouseButtonFlags b;
    int held = SDL_GetAtomicInt(&mouse_held);

    if (!held)
        return 0;
    b = SDL_GetMouseState(NULL, NULL);
    if (held == 2) {
        if (b)
            return 0;
        SDL_CompareAndSwapAtomicInt(&mouse_held, 2, 1);
    }
    return b;
}

static const char *const mouse_names[] = { "-", "left", "middle", "right", "x1", "x2" };

const char *mdr_input_pad_label(int pad)
{
    static const struct { int pad; const char *label; } labels[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, "A (south)" }, { SDL_GAMEPAD_BUTTON_EAST, "B (east)" },
        { SDL_GAMEPAD_BUTTON_WEST, "X (west)" }, { SDL_GAMEPAD_BUTTON_NORTH, "Y (north)" },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB" }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB" },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, "LS click" }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RS click" },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, "D-up" }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-down" },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-left" }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-right" },
        { SDL_GAMEPAD_BUTTON_START, "Start" }, { SDL_GAMEPAD_BUTTON_BACK, "Back" },
        { SDL_GAMEPAD_BUTTON_GUIDE, "Guide" },
        { MDR_PAD_AXIS + SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "LT" }, { MDR_PAD_AXIS + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "RT" },
    };
    for (size_t i = 0; i < SDL_arraysize(labels); i++)
        if (labels[i].pad == pad)
            return labels[i].label;
    return mdr_input_pad_name(pad);
}

const char *mdr_input_mouse_name(int mouse)
{
    return mouse >= 1 && mouse <= 5 ? mouse_names[mouse] : "-";
}

void mdr_mouse_add(float dx, float dy)
{
    SDL_LockSpinlock(&mouse_lock);
    mouse_dx += dx;
    mouse_dy += dy;
    SDL_UnlockSpinlock(&mouse_lock);
}

void mdr_mouse_take(float *dx, float *dy)
{
    SDL_LockSpinlock(&mouse_lock);
    *dx = mouse_dx;
    *dy = mouse_dy;
    mouse_dx = mouse_dy = 0.0f;
    SDL_UnlockSpinlock(&mouse_lock);
}
static bool ready;

mdr_binding *mdr_input_bindings(void)
{
    if (!ready)
        mdr_input_reset();
    return bindings;
}

const char *mdr_input_button_name(int b)
{
    return b >= 0 && b < MDR_IN_COUNT ? buttons[b].name : "?";
}

void mdr_input_reset(void)
{
    for (int i = 0; i < MDR_IN_COUNT; i++)
        bindings[i] = buttons[i].def;
    ready = true;
}

int mdr_input_pad_level(SDL_Gamepad *g, int pad)
{
    if (!g || pad < 0)
        return 0;
    if (pad >= MDR_PAD_AXIS) {      /* triggers are 0..32767 in SDL */
        int v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)(pad - MDR_PAD_AXIS));
        return v > 0 ? v * 255 / 32767 : 0;
    }
    return SDL_GetGamepadButton(g, (SDL_GamepadButton)pad) ? 255 : 0;
}

const char *mdr_input_pad_name(int pad)
{
    const char *s = pad < 0 ? NULL
                  : pad >= MDR_PAD_AXIS ? SDL_GetGamepadStringForAxis((SDL_GamepadAxis)(pad - MDR_PAD_AXIS))
                  : SDL_GetGamepadStringForButton((SDL_GamepadButton)pad);
    return s ? s : "-";
}

bool mdr_input_load_line(const char *name, const char *value)
{
    bool key = strncmp(name, "key_", 4) == 0, pad = strncmp(name, "pad_", 4) == 0;
    bool mouse = strncmp(name, "mouse_", 6) == 0;

    if (mouse) {
        mdr_input_bindings();
        for (int i = 0; i < MDR_IN_COUNT; i++)
            if (strcmp(name + 6, buttons[i].ini) == 0) {
                bindings[i].mouse = 0;
                for (int m = 1; m <= 5; m++)
                    if (strcmp(value, mouse_names[m]) == 0)
                        bindings[i].mouse = m;
            }
        return true;
    }
    if (!key && !pad)
        return false;
    mdr_input_bindings();
    for (int i = 0; i < MDR_IN_COUNT; i++) {
        if (strcmp(name + 4, buttons[i].ini) != 0)
            continue;
        if (key) {
            bindings[i].key = strcmp(value, "-") == 0 ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(value);
        } else {
            SDL_GamepadButton b = SDL_GetGamepadButtonFromString(value);
            SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(value);
            bindings[i].pad = b != SDL_GAMEPAD_BUTTON_INVALID ? (int)b
                            : a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                ? MDR_PAD_AXIS + (int)a : -1;
        }
        return true;
    }
    return true;    /* a key_/pad_ line for no button we know: ignored */
}

size_t mdr_input_save(char *out, size_t cap)
{
    size_t n = 0;

    mdr_input_bindings();
    for (int i = 0; i < MDR_IN_COUNT && n < cap; i++) {
        const char *k = bindings[i].key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(bindings[i].key) : "";
        int w = snprintf(out + n, cap - n, "key_%s=%s\npad_%s=%s\nmouse_%s=%s\n", buttons[i].ini, k[0] ? k : "-",
                         buttons[i].ini, mdr_input_pad_name(bindings[i].pad), buttons[i].ini,
                         mdr_input_mouse_name(bindings[i].mouse));
        if (w < 0)
            break;
        n += (size_t)w;
    }
    return n < cap ? n : cap;
}
