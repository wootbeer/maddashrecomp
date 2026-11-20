#ifndef MDR_INPUT_MAP_H
#define MDR_INPUT_MAP_H

/*
 * Which key and which controller button play each Xbox button, changed in
 * the options overlay (Controls) and saved with the settings. The sticks
 * stay where they are: the left and right sticks, and WASD/IJKL on the
 * keyboard.
 */

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    MDR_IN_A, MDR_IN_B, MDR_IN_X, MDR_IN_Y, MDR_IN_BLACK, MDR_IN_WHITE, MDR_IN_LTRIG, MDR_IN_RTRIG,
    MDR_IN_START, MDR_IN_BACK, MDR_IN_UP, MDR_IN_DOWN, MDR_IN_LEFT, MDR_IN_RIGHT, MDR_IN_LTHUMB, MDR_IN_RTHUMB,
    MDR_IN_COUNT
};

/* A controller input: a button (SDL_GamepadButton), a trigger
 * (MDR_PAD_AXIS + SDL_GamepadAxis), or none (-1). */
#define MDR_PAD_AXIS 100

typedef struct mdr_binding {
    SDL_Scancode key;       /* SDL_SCANCODE_UNKNOWN: none */
    int          pad;
    int          mouse;     /* SDL_BUTTON_LEFT..X2, 0: none */
} mdr_binding;

mdr_binding *mdr_input_bindings(void);          /* MDR_IN_COUNT of them */
const char *mdr_input_button_name(int b);       /* "A", "Start", ... */
void mdr_input_reset(void);                     /* the defaults */
/* How far a binding's controller input is pressed, 0..255 (triggers are
 * analog, buttons 0 or 255); 0 without a controller. */
int mdr_input_pad_level(SDL_Gamepad *g, int pad);
const char *mdr_input_pad_name(int pad);        /* "a", "lefttrigger", "-" */
const char *mdr_input_mouse_name(int mouse);    /* "left", ... "-" */
const char *mdr_input_pad_label(int pad);       /* short, for the menu: "A", "RB", "LT" */

/* Mouse motion while the game has the mouse (renderer.c adds it, in window
 * pixels; input_hle.c takes it once a game frame). */
void mdr_mouse_add(float dx, float dy);
void mdr_mouse_take(float *dx, float *dy);
/* Whether the game has the mouse (held in the window): mouse buttons play
 * the game only then, so the click that takes the mouse back does not. */
void mdr_mouse_set_captured(bool on);
SDL_MouseButtonFlags mdr_mouse_buttons(void);    /* none while not captured */

/* Settings file lines (config.c): key_<button>=<key name>,
 * pad_<button>=<controller input name>. */
bool mdr_input_load_line(const char *name, const char *value);
size_t mdr_input_save(char *out, size_t cap);

#endif
