/*
 * Widescreen for Mad Dash Racing.
 *
 * The game has no 16:9 mode of its own: it never asks the console for its
 * video flags (its only ExQueryNonVolatileSetting calls read the misc flags
 * and the time zone). Its views come from one camera object, built by
 * 0x000D4000 (thiscall: field of view, aspect, near, far; `ret 0x10`;
 * vtable 0x002A8F88), with 4/3 (0x3FAAAAAB) for the aspect at its callers
 * (0x00119B58, 0x00188023, 0x00188153). The camera keeps it at +0x58 (field
 * of view +0x54, near +0x5C, far +0x60), and the view is set up from it
 * every frame (0x001E6A90, through the getter 0x000D3C50).
 *
 * With the Aspect setting at 16:9 or 21:9, the cameras made for 4:3 get that
 * aspect while a race is on screen (d3d_hle.c: its progress line drawn): the
 * game renders its 640x480 frame anamorphic and the window pass shows it at
 * that shape (renderer.c, the frame's ratio), the HUD squeezed back to its
 * own proportions (d3d_hle.c, hud_squeeze_of). Its culling works from the
 * same camera, so what is now in view is drawn. Menus and loading screens
 * stay at 4:3 with bars at the sides: the game places some menu text by
 * projecting points of the menu's 3D frames through the camera, and with a
 * wider camera and the 2D squeezed as well, that text came out squeezed
 * twice (xemu's character select against ours).
 */

#include <SDL3/SDL.h>
#include <string.h>

#include "recomp_types.h"
#include "config.h"

#define CAMERA_ASPECT 0x58u
#define CAMERA_VTABLE 0x002A8F88u
#define MAX_CAMERAS   16

static uint32_t cameras[MAX_CAMERAS];   /* those made for 4:3 */
static int ncameras;
static bool wide;                       /* the cameras are wide now (a race, a wide setting) */
static bool drawn_wide;                 /* and were while the frame just finished was drawn */
static int wide_aspect = MDR_ASPECT_16_9;

static float ratio_of(int aspect)
{
    return aspect == MDR_ASPECT_21_9 ? 64.0f / 27.0f : aspect == MDR_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

bool mdr_widescreen_active(void)
{
    return wide;
}

float mdr_widescreen_ratio(void)
{
    return wide ? ratio_of(wide_aspect) : 4.0f / 3.0f;
}

float mdr_widescreen_frame_ratio(void)
{
    return drawn_wide ? ratio_of(wide_aspect) : 4.0f / 3.0f;
}

bool mdr_widescreen_needs_restart(void)
{
    return false;
}

/* Game thread, at Present: race says a race is on screen. The cameras are
 * set for the frames from here on (the view is set up from them each
 * frame); a camera's memory is written only while it still holds a camera. */
void mdr_widescreen_race(bool race)
{
    int want = mdr_settings_get()->aspect;
    bool now = race && want != MDR_ASPECT_4_3;
    float r = now ? ratio_of(want) : 4.0f / 3.0f;

    drawn_wide = wide;
    if (now != wide || (now && want != wide_aspect))
        SDL_Log("widescreen: cameras at %s", now ? (want == MDR_ASPECT_21_9 ? "21:9" : "16:9") : "4:3");
    wide = now;
    if (now)
        wide_aspect = want;
    for (int i = 0; i < ncameras; i++)
        if (MEM32(cameras[i]) == CAMERA_VTABLE)
            MEMF(cameras[i] + CAMERA_ASPECT) = r;
}

/* MDR_WIDE_SKIP=addr[,addr...]: cameras made from these return addresses
 * keep 4:3 (finding which camera a stretched effect comes from). */
static bool skipped(uint32_t ret)
{
    const char *e = SDL_getenv("MDR_WIDE_SKIP");
    while (e && *e) {
        char *end;
        unsigned long a = SDL_strtoul(e, &end, 16);
        if (end == e)
            break;
        if ((uint32_t)a == ret)
            return true;
        e = *end ? end + 1 : end;
    }
    return false;
}

/* The camera's constructor (this in ecx; field of view, aspect, near, far):
 * a camera made for 4:3 is remembered, and gets the aspect in force. Each
 * caller's first camera is logged (its return address, for MDR_WIDE_SKIP). */
extern void sub_000D4000_gen(void);
void sub_000D4000(void)
{
    static uint32_t seen[16];
    static int nseen;
    uint32_t cam = g_ecx, ret = MEM32(g_esp);
    float fov, asked, nearz, farz;
    bool for43, logged = false;

    memcpy(&fov, (const void *)XBOX_PTR(g_esp + 4), 4);
    memcpy(&asked, (const void *)XBOX_PTR(g_esp + 8), 4);
    memcpy(&nearz, (const void *)XBOX_PTR(g_esp + 12), 4);
    memcpy(&farz, (const void *)XBOX_PTR(g_esp + 16), 4);
    sub_000D4000_gen();
    for43 = asked > 1.33f && asked < 1.34f && !skipped(ret);
    for (int i = 0; i < nseen; i++)
        logged |= seen[i] == ret;
    if (!logged && nseen < 16) {
        seen[nseen++] = ret;
        SDL_Log("widescreen: camera from %08X: fov %g aspect %g near %g far %g%s", ret, fov, asked, nearz, farz,
                for43 ? " (follows the aspect setting in races)" : "");
    }
    if (for43) {
        bool known = false;
        for (int i = 0; i < ncameras; i++)
            known |= cameras[i] == cam;
        if (!known && ncameras < MAX_CAMERAS)
            cameras[ncameras++] = cam;
        if (wide)
            MEMF(cam + CAMERA_ASPECT) = ratio_of(wide_aspect);
    }
}
