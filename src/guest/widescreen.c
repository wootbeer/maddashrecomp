/*
 * Widescreen for Mad Dash Racing: not found yet.
 *
 * Orta Recomp hooked its game's own 16:9 mode and widened the view for
 * 21:9 (its src/guest/widescreen.c). Mad Dash's display setup, projection
 * and HUD layout still have to be found; until then the game runs as the
 * console did (4:3, or whatever its own settings pick), and the Aspect
 * setting only changes how the picture is fitted to the window.
 */

#include "config.h"

bool mdr_widescreen_active(void) { return false; }
float mdr_widescreen_ratio(void) { return 4.0f / 3.0f; }
bool mdr_widescreen_needs_restart(void) { return false; }
void mdr_widescreen_frame(void) {}
