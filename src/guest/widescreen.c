/*
 * Widescreen for Mad Dash Racing.
 *
 * The game has no 16:9 mode of its own: it never asks the console for its
 * video flags (its only ExQueryNonVolatileSetting calls read the misc flags
 * and the time zone). Its views come from one camera object, built by
 * 0x000D4000 (thiscall: field of view, aspect, near, far; `ret 0x10`;
 * vtable 0x002A8F88), with 4/3 (0x3FAAAAAB) for the aspect at its callers
 * (0x00119B58, 0x00188023, 0x00188153). The camera keeps it at +0x58 (field
 * of view +0x54, near +0x5C, far +0x60).
 *
 * With the Aspect setting at 16:9 or 21:9, a camera made from then on gets
 * that aspect instead: the game renders its 640x480 frame anamorphic and
 * the window pass shows it at that shape (renderer.c, mdr_widescreen_ratio).
 * Its culling works from the same camera, so what is now in view is drawn.
 * The setting takes effect with the next camera the game makes (the next
 * level or menu); until then the picture keeps the shape it was made for.
 */

#include <SDL3/SDL.h>
#include <string.h>

#include "recomp_types.h"
#include "config.h"

#define CAMERA_ASPECT 0x58u

static int made_for = MDR_ASPECT_4_3;   /* the aspect of the cameras made last */

static float ratio_of(int aspect)
{
    return aspect == MDR_ASPECT_21_9 ? 64.0f / 27.0f : aspect == MDR_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

bool mdr_widescreen_active(void)
{
    return made_for != MDR_ASPECT_4_3;
}

float mdr_widescreen_ratio(void)
{
    return ratio_of(made_for);
}

bool mdr_widescreen_needs_restart(void)
{
    return made_for != mdr_settings_get()->aspect;
}

void mdr_widescreen_frame(void) {}

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
 * a camera made for 4:3 gets the setting's aspect. Each caller's first
 * camera is logged (its return address, for MDR_WIDE_SKIP). */
extern void sub_000D4000_gen(void);
void sub_000D4000(void)
{
    static uint32_t seen[16];
    static int nseen;
    uint32_t cam = g_ecx, ret = MEM32(g_esp);
    float fov, asked, nearz, farz;
    int want = mdr_settings_get()->aspect;
    bool widen, logged = false;

    memcpy(&fov, (const void *)XBOX_PTR(g_esp + 4), 4);
    memcpy(&asked, (const void *)XBOX_PTR(g_esp + 8), 4);
    memcpy(&nearz, (const void *)XBOX_PTR(g_esp + 12), 4);
    memcpy(&farz, (const void *)XBOX_PTR(g_esp + 16), 4);
    sub_000D4000_gen();
    widen = asked > 1.33f && asked < 1.34f && want != MDR_ASPECT_4_3 && !skipped(ret);
    for (int i = 0; i < nseen; i++)
        logged |= seen[i] == ret;
    if (!logged && nseen < 16) {
        seen[nseen++] = ret;
        SDL_Log("widescreen: camera from %08X: fov %g aspect %g near %g far %g%s", ret, fov, asked, nearz, farz,
                widen ? " -> widened" : "");
    }
    if (widen) {
        MEMF(cam + CAMERA_ASPECT) = ratio_of(want);
        if (made_for != want)
            SDL_Log("widescreen: cameras made at %s", want == MDR_ASPECT_21_9 ? "21:9" : "16:9");
    } else if (want == MDR_ASPECT_4_3 && made_for != want) {
        SDL_Log("widescreen: cameras made at 4:3");
    }
    made_for = want;
}
