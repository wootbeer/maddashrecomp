/*
 * DirectSound for Mad Dash Racing, replaced at its API.
 *
 * Carried over from Orta Recomp's src/hle/dsound_hle.c (first written for
 * JSRF), re-addressed for Mad Dash's DSOUND 3936 from XbSymbolDatabase
 * names; every `ret N` checked in Mad Dash's own code. Where 3936 differs
 * from Orta's 4928 it says so: mix bins are bit masks (bit n = mix bin n)
 * in DSBUFFERDESC, DSSTREAMDESC and SetMixBins, not DSMIXBINS lists; the
 * 3D filters are the "light" HRTF (azimuth only).
 *
 * The title links the XDK's DirectSound library, which drives the Xbox's
 * MCPX audio processor directly; with no such hardware its voice lists are
 * never set up and CMcpxAPU_Commit3dSettings loops for ever. Every
 * DirectSound entry point the game (and Bink's audio) calls is replaced
 * here and routed to our mixer (src/audio/mixer.c). The library's
 * internals are then never reached.
 *
 * Conventions: each function is __stdcall. At entry g_esp points at the
 * return address and argument n is at g_esp + 4 + 4n. Each one pops the
 * return address and exactly the argument bytes the original's `ret N` did
 * (measured from the binary), and returns its HRESULT in g_eax.
 *
 * Objects handed to the game are small blocks of guest memory holding a tag
 * and our ids; the game treats them as opaque interface pointers.
 *
 * Not implemented yet, accepted and recorded: the DSP image's own effects
 * (its I3DL2 reverb is ours, in src/audio/reverb.c), I3DL2 source HF
 * levels, obstruction and occlusion, 3D streams.
 *
 * MDR_DSOUND_TRACE=1 logs every call; MDR_HRTF=n sets the HRTF filters'
 * scale (below).
 */

#include "recomp_types.h"

#include "audio/mixer.h"
#include "config.h"
#include "guest/guest.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG(n)      MEM32(g_esp + 4 + 4 * (n))
#define RETURN(argbytes, hr) do { g_eax = (uint32_t)(hr); g_esp += 4 + (argbytes); return; } while (0)

#define DS_OK               0x00000000u
#define DSERR_INVALIDCALL   0x88780032u
#define DSERR_OUTOFMEMORY   0x8007000Eu

#define TAG_DSOUND  0x534F4450u     /* "PDOS" */
#define TAG_BUFFER  0x464F4450u     /* "PDOF" */

/* Object layout in guest memory. */
#define OBJ_TAG     0
#define OBJ_VOICE   4
#define OBJ_REFS    8
#define OBJ_DATA    12   /* the buffer's memory: ours, or the game's (SetBufferData) */
#define OBJ_BYTES   16
#define OBJ_OWN     20   /* memory we allocated for it, freed with it */
#define OBJ_SIZE    32

static uint32_t the_dsound;

/* 3D state of DSBCAPS_CTRL3D buffers, by voice id (see "3D sound"). */
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE      2
#define MAX_3D                513     /* voice ids are 1..512 */

static struct {
    float pos[3], front[3], top[3], rolloff;
} listener = { {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.0f };

static struct {
    bool  used;
    float pos[3], min_dist, max_dist;
    uint32_t mode;
} src3d[MAX_3D];

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("MDR_DSOUND_TRACE");
        on = v && *v && *v != '0';
    }
    return on;
}

#define TRACE(...) do { if (trace_on()) SDL_Log("dsound: " __VA_ARGS__); } while (0)

static void note_once(const char *what)
{
    /* Each unimplemented feature is reported the first time it is used. */
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what)
            return;
        if (!seen[i]) {
            seen[i] = what;
            SDL_Log("dsound: %s used by the game (accepted, not implemented yet)", what);
            return;
        }
    }
}

static int voice_of(uint32_t obj)
{
    if (!obj || MEM32(obj + OBJ_TAG) != TAG_BUFFER)
        return 0;
    return (int)MEM32(obj + OBJ_VOICE);
}

static uint32_t guest_alloc(uint32_t bytes)
{
    uint32_t va = xbox_HeapAlloc(bytes ? bytes : 4, 32);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, bytes);
    return va;
}

/* Mix bins from a 3936 mask: bit n is mix bin n (DSMIXBIN_FRONT_LEFT 0 ...
 * XTLK 6-9, I3DL2 10, FXSEND 11 on), each at 0 dB. As the library sets a
 * voice up (0x00283162): a DSBCAPS_CTRL3D voice always gets 0x7C0 (the
 * crosstalk bins and I3DL2), mask 0 the default for its channel count
 * from the table at 0x0028BF68 (mono 0x3F: all six speakers; stereo 0x3;
 * 3: 0x7; 4: 0x33; 5: 0x37; 6: 0x3F). */
static int bins_from_mask(uint32_t mask, bool positional, int channels, mdr_voice_bin *bins)
{
    static const uint32_t defaults[8] = { 0x0, 0x3F, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x0 };
    int n = 0;

    if (positional)
        mask = 0x7C0;
    else if (!mask)
        mask = defaults[channels & 7];
    for (int bin = 0; bin < 32 && n < MDR_MAX_VOICE_BINS; bin++)
        if (mask & (1u << bin)) {
            bins[n].bin = (uint32_t)bin;
            bins[n].volume = 0;
            n++;
        }
    return n;
}

/* ── IDirectSound ─────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDS, pUnkOuter) */
void sub_00284671(void)
{
    uint32_t pp = ARG(1);

    if (!the_dsound) {
        mdr_mixer_init();
        the_dsound = guest_alloc(OBJ_SIZE);
        if (!the_dsound)
            RETURN(12, DSERR_OUTOFMEMORY);
        MEM32(the_dsound + OBJ_TAG) = TAG_DSOUND;
    }
    MEM32(the_dsound + OBJ_REFS) += 1;
    if (pp)
        MEM32(pp) = the_dsound;
    TRACE("DirectSoundCreate -> 0x%08x", the_dsound);
    RETURN(12, DS_OK);
}

/* IDirectSound_Release(pDS). The device lives for the whole run. */
void sub_00282F63(void)
{
    uint32_t ds = ARG(0), refs = 0;
    if (ds && MEM32(ds + OBJ_REFS))
        refs = (MEM32(ds + OBJ_REFS) -= 1);
    RETURN(4, refs);
}

/* IDirectSound_CreateSoundBuffer(pDS, pdsbd, ppBuffer, pUnkOuter) */
void sub_0028451D(void)
{
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags, bytes, wfx, mixbins, obj, data = 0;
    mdr_voice_desc vd;
    int id;

    if (!desc || !pp)
        RETURN(16, DSERR_INVALIDCALL);
    flags = MEM32(desc + 4);
    bytes = MEM32(desc + 8);
    wfx = MEM32(desc + 12);
    mixbins = MEM32(desc + 16);         /* dwMixBinMask (3936) */

    memset(&vd, 0, sizeof vd);
    vd.format = MDR_FMT_PCM16;
    vd.channels = 1;
    vd.sample_rate = 48000;
    vd.positional = (flags & 0x10) != 0;    /* DSBCAPS_CTRL3D */
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 2 ? 2 : 1;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = MDR_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? MDR_FMT_PCM8 : MDR_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: unknown wave format 0x%04x, treating as PCM", tag);
        }
    }

    vd.bin_count = bins_from_mask(mixbins, vd.positional, wfx ? MEM16(wfx + 2) : 1, vd.bins);

    if (bytes) {
        data = guest_alloc(bytes);
        if (!data)
            RETURN(16, DSERR_OUTOFMEMORY);
        vd.data = data;
        vd.bytes = bytes;
    }

    id = mdr_voice_create(&vd);
    obj = guest_alloc(OBJ_SIZE);
    if (!id || !obj)
        RETURN(16, DSERR_OUTOFMEMORY);
    MEM32(obj + OBJ_TAG) = TAG_BUFFER;
    if (id < MAX_3D) {
        memset(&src3d[id], 0, sizeof src3d[id]);
        src3d[id].used = vd.positional;
    }
    MEM32(obj + OBJ_VOICE) = (uint32_t)id;
    MEM32(obj + OBJ_REFS) = 1;
    MEM32(obj + OBJ_DATA) = data;
    MEM32(obj + OBJ_BYTES) = bytes;
    MEM32(obj + OBJ_OWN) = data;
    MEM32(pp) = obj;

    TRACE("CreateSoundBuffer voice %d", id);
    TRACE("CreateSoundBuffer flags 0x%x, %u bytes, %s %u ch %u Hz, %d bins -> 0x%08x",
          flags, bytes, vd.format == MDR_FMT_XADPCM ? "adpcm" : vd.format == MDR_FMT_PCM8 ? "pcm8" : "pcm16",
          vd.channels, vd.sample_rate, vd.bin_count, obj);
    for (int i = 0; i < vd.bin_count; i++)
        TRACE("  0x%08x bin %u volume %d", obj, vd.bins[i].bin, vd.bins[i].volume);
    RETURN(16, DS_OK);
}

/* DirectSoundUseLightHRTF() (called once during sound init): selects the
 * light HRTF tables, which apply_3d reads itself (hrtf_filters). Nothing
 * to set up; takes no arguments. */
void sub_00282F94(void)
{
    g_esp += 4;
}

/* DirectSoundDoWork(): the mixer runs on its own thread. */
void sub_00283A21(void)
{
    g_esp += 4;
}

/* IDirectSound_CommitDeferredSettings(pDS) */
void sub_00283872(void) { RETURN(4, DS_OK); }

/* ── DSP effects image ─────────────────────────────────────────────────
 * The title loads a DSP program (reverb and the like) and then reads and
 * writes its parameters by effect index. We return a descriptor whose state
 * segments are plain memory, so parameter traffic works, and record it. */

#define FX_COUNT       32
#define FX_STATE_BYTES 4096
static uint32_t fx_desc;

/* IDirectSound_DownloadEffectsImage(pDS, pvImage, dwSize, pImageLoc, ppDesc) */
void sub_0028382B(void)
{
    uint32_t pp = ARG(4);

    note_once("DSP effects image (reverb)");
    TRACE("DownloadEffectsImage %u bytes at 0x%08x, image loc 0x%08x", ARG(2), ARG(1), ARG(3));
    if (!fx_desc) {
        fx_desc = guest_alloc(8 + FX_COUNT * 32);
        if (!fx_desc)
            RETURN(20, DSERR_OUTOFMEMORY);
        MEM32(fx_desc) = FX_COUNT;
        for (int i = 0; i < FX_COUNT; i++) {
            uint32_t map = fx_desc + 8 + (uint32_t)i * 32;
            MEM32(map + 8) = guest_alloc(FX_STATE_BYTES);    /* lpvStateSegment */
            MEM32(map + 12) = FX_STATE_BYTES;
        }
    }
    if (pp)
        MEM32(pp) = fx_desc;
    RETURN(20, DS_OK);
}

/* ── 3D sound ─────────────────────────────────────────────────────────
 * DSBCAPS_CTRL3D buffers are placed relative to the listener: DirectSound's
 * distance model (full volume up to the minimum distance, then
 * min / (min + rolloff * (d - min)), held from the maximum distance on),
 * then the APU's per-voice HRTF filter from the game's own tables (below;
 * stereo 3D buffers are panned), on 5.1 shared between the front and back
 * speaker pairs as DirectSound does. With 3D sound off: an equal-power pan,
 * on Headphones a model head, on 5.1 a pan around the four corners. No cones or Doppler
 * (the game calls neither). Deferred settings apply at once. */

static float fget(uint32_t va)
{
    uint32_t v = MEM32(va);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float fval(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Sound > 3D sound On (the default): mono 3D voices go through the Xbox's
 * own HRTF filters, as the APU placed them; Off pans them (equal power, on
 * Headphones the model head, on 5.1 around the corners). The APU's scale
 * for the taps is not known: / 512 sounded closest to the console by ear in
 * Orta Recomp; MDR_HRTF=n divides by n instead, MDR_HRTF=0 is the same as
 * Off. Mad Dash selects the light HRTF (DirectSoundUseLightHRTF), looked up
 * by CSensaura3d_GetLiteHRTFFilterPair (0x00287BF5): azimuth only, |az| +
 * 1.5 truncated to a multiple of 3 degrees (0..180, else 0), on 5.1 a sound
 * behind folded onto the front, then the pair (180 - a) / 3 of 64 bytes at
 * 0x0028C118. An entry is 31 sign-magnitude taps (the newest sample first)
 * and, in the pair's first entry, the delay in 48 kHz samples. The first
 * entry is the ear away from the sound, the second the ear facing it
 * (sent by the azimuth's sign). */
static float hrtf_scale(void)
{
    static float scale = -1.0f;
    if (scale < 0.0f) {
        const char *e = getenv("MDR_HRTF");
        scale = e ? (float)(atoi(e) > 0 ? atoi(e) : 0) : 512.0f;
        if (e)
            SDL_Log("dsound: MDR_HRTF: %s %g", scale > 0.0f ? "HRTF taps /" : "off,", scale);
    }
    return scale;
}

#define LITE_HRTF_PAIRS 0x0028C118u

/* az: degrees, positive to the right. */
static void hrtf_filters(float az, float scale, bool surround, float h[2][MDR_HRTF_TAPS], float delay[2])
{
    int a = (int)(fabsf(az) + 1.5f) / 3 * 3;
    uint32_t pair;

    if (a < 0 || a > 180)
        a = 0;
    if (surround && a > 90)
        a = 180 - a;
    pair = LITE_HRTF_PAIRS + (uint32_t)((180 - a) / 3) * 64u;
    for (int k = 0; k < 2; k++) {
        /* k 0: the far ear (the first entry), 1: the near ear */
        int ear = (az >= 0.0f) == (k == 0) ? 0 : 1;
        uint32_t ent = pair + 32u * (uint32_t)k;
        for (int i = 0; i < MDR_HRTF_TAPS; i++) {
            uint8_t b = MEM8(ent + (uint32_t)i);
            h[ear][i] = (float)(b & 0x80 ? -(int)(b & 0x7F) : (int)b) / scale;
        }
        delay[ear] = k == 0 ? (float)(MEM8(pair + 31u) & 0x7F) : 0.0f;
    }
}

/* On 5.1, DirectSound shares a 3D voice's power between the front and
 * back speaker pairs (as Orta's 4928 does; not yet checked in 3936's code;
 * the APU's HRTF filter does left and right): the back share is (1 - |el| / 90) (|az| / 90 - 1) + 0.5, held to
 * 0..1 -- all front within 45 degrees of ahead, all back within 45 of
 * behind, even overhead. A voice nearer than half a unit (its distance
 * times the listener's distance factor and a per-buffer scale, taken as 1:
 * Orta's were 1 and the scale is internal) is drawn towards even, by half
 * its distance. Each share becomes a volume, 1000 log10 (share). */
static float back_share(float az, float el, float d)
{
    float v = (1.0f - fabsf(el) / 90.0f) * (fabsf(az) / 90.0f - 1.0f) + 0.5f;
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    if (d < 0.5f)
        v = 0.5f + (v - 0.5f) * d * 0.5f;
    return v;
}

static void apply_3d(int id)
{
    float rel[3], right[3], d, g, x = 0.0f, l, r;
    const float *f = listener.front, *t = listener.top;

    if (id <= 0 || id >= MAX_3D || !src3d[id].used)
        return;
    if (src3d[id].mode == DS3DMODE_DISABLE) {
        mdr_voice_set_3d_distance(id, 1.0f);
        mdr_voice_set_3d(id, 1.0f, 1.0f);
        return;
    }
    for (int k = 0; k < 3; k++)
        rel[k] = src3d[id].pos[k] - (src3d[id].mode == DS3DMODE_HEADRELATIVE ? 0.0f : listener.pos[k]);
    d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    {
        float mn = src3d[id].min_dist > 0 ? src3d[id].min_dist : 1.0f;
        float mx = src3d[id].max_dist > mn ? src3d[id].max_dist : 1e9f;
        float dc = d < mn ? mn : d > mx ? mx : d;
        g = mn / (mn + listener.rolloff * (dc - mn));
    }
    mdr_voice_set_3d_distance(id, g);
    /* Right = top x front (D3D is left-handed); head-relative uses the
     * listener's own axes, which are the identity there. */
    if (src3d[id].mode == DS3DMODE_HEADRELATIVE) {
        right[0] = 1; right[1] = 0; right[2] = 0;
    } else {
        right[0] = t[1] * f[2] - t[2] * f[1];
        right[1] = t[2] * f[0] - t[0] * f[2];
        right[2] = t[0] * f[1] - t[1] * f[0];
    }
    if (d > 1e-6f) {
        float n = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (n > 1e-6f)
            x = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / (d * n);
    }
    if (mdr_settings_get()->hrtf && hrtf_scale() > 0.0f) {
        const float fr[3] = { 0.0f, 0.0f, 1.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
        const bool hr = src3d[id].mode == DS3DMODE_HEADRELATIVE;
        const float *ff = hr ? fr : f, *tt = hr ? up : t;
        float fz = 0.0f, y = 0.0f, h[2][MDR_HRTF_TAPS], delay[2];
        float nf = sqrtf(ff[0] * ff[0] + ff[1] * ff[1] + ff[2] * ff[2]);
        float nt = sqrtf(tt[0] * tt[0] + tt[1] * tt[1] + tt[2] * tt[2]);
        if (d > 1e-6f && nf > 1e-6f)
            fz = (rel[0] * ff[0] + rel[1] * ff[1] + rel[2] * ff[2]) / (d * nf);
        if (d > 1e-6f && nt > 1e-6f)
            y = (rel[0] * tt[0] + rel[1] * tt[1] + rel[2] * tt[2]) / (d * nt);
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        {
            float az = atan2f(x, fz) * 57.2957795f, el = asinf(y) * 57.2957795f, back = 0.0f;
            bool surround = mdr_settings_get()->audio_out == MDR_AUDIO_SURROUND;
            if (surround)
                back = back_share(az, el, d);
            hrtf_filters(az, hrtf_scale(), surround, h, delay);
            mdr_voice_set_3d_hrtf(id, g, h, delay, sqrtf(1.0f - back), sqrtf(back));
        }
        return;
    }
    /* Headphones (a model head) and 5.1 (around four speakers), the output
     * setting: both need ahead or behind too. */
    if (mdr_settings_get()->audio_out != MDR_AUDIO_STEREO) {
        float fz = 0.0f;
        const float fr[3] = { 0.0f, 0.0f, 1.0f };
        const float *ff = src3d[id].mode == DS3DMODE_HEADRELATIVE ? fr : f;
        float n = sqrtf(ff[0] * ff[0] + ff[1] * ff[1] + ff[2] * ff[2]);
        if (d > 1e-6f && n > 1e-6f)
            fz = (rel[0] * ff[0] + rel[1] * ff[1] + rel[2] * ff[2]) / (d * n);
        if (mdr_settings_get()->audio_out == MDR_AUDIO_SURROUND)
            mdr_voice_set_3d_surround(id, g, x, fz);
        else
            mdr_voice_set_3d_ears(id, g, x, fz);
        return;
    }
    /* Equal-power pan: a source straight ahead is -3 dB in each ear. */
    l = sqrtf(0.5f * (1.0f - x));
    r = sqrtf(0.5f * (1.0f + x));
    mdr_voice_set_3d(id, g * l, g * r);
}

static void apply_3d_all(void)
{
    for (int id = 1; id < MAX_3D; id++)
        if (src3d[id].used)
            apply_3d(id);
}

/* IDirectSound_SetAllParameters(pDS, pds3dl, dwApply). DS3DLISTENER:
 * dwSize, vPosition, vVelocity, vOrientFront, vOrientTop,
 * flDistanceFactor, flRolloffFactor, flDopplerFactor. */
void sub_00283852(void)
{
    uint32_t l = ARG(1);
    if (l) {
        for (int k = 0; k < 3; k++) {
            listener.pos[k] = fget(l + 4 + 4 * k);
            listener.front[k] = fget(l + 28 + 4 * k);
            listener.top[k] = fget(l + 40 + 4 * k);
        }
        listener.rolloff = fget(l + 56);
        TRACE("listener pos %g %g %g front %g %g %g rolloff %g", listener.pos[0], listener.pos[1],
              listener.pos[2], listener.front[0], listener.front[1], listener.front[2], listener.rolloff);
        apply_3d_all();
    }
    RETURN(12, DS_OK);
}

/* IDirectSound_SetI3DL2Listener(pDS, pds3dl, dwApply). DSI3DL2LISTENER:
 * lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime, flDecayHFRatio,
 * lReflections, flReflectionsDelay, lReverb, flReverbDelay, flDiffusion,
 * flDensity, flHFReference. */
void sub_0028388A(void)
{
    uint32_t p = ARG(1);

    if (p) {
        mdr_i3dl2 env = {
            .room = (int32_t)MEM32(p), .room_hf = (int32_t)MEM32(p + 4),
            .room_rolloff = fval(MEM32(p + 8)), .decay_time = fval(MEM32(p + 12)),
            .decay_hf_ratio = fval(MEM32(p + 16)), .reflections = (int32_t)MEM32(p + 20),
            .reflections_delay = fval(MEM32(p + 24)), .reverb = (int32_t)MEM32(p + 28),
            .reverb_delay = fval(MEM32(p + 32)), .diffusion = fval(MEM32(p + 36)),
            .density = fval(MEM32(p + 40)), .hf_reference = fval(MEM32(p + 44)),
        };
        mdr_mixer_set_i3dl2(&env);
    }
    if (p)
        TRACE("SetI3DL2Listener room %d hf %d rolloff %.2f decay %.2f s hf ratio %.2f "
              "reflections %d delay %.3f reverb %d delay %.3f diffusion %.0f density %.0f "
              "hf ref %.0f", (int32_t)MEM32(p), (int32_t)MEM32(p + 4), fval(MEM32(p + 8)),
              fval(MEM32(p + 12)), fval(MEM32(p + 16)), (int32_t)MEM32(p + 20),
              fval(MEM32(p + 24)), (int32_t)MEM32(p + 28), fval(MEM32(p + 32)),
              fval(MEM32(p + 36)), fval(MEM32(p + 40)), fval(MEM32(p + 44)));
    RETURN(12, DS_OK);
}

/* ── IDirectSoundBuffer ───────────────────────────────────────────────── */

/* IDirectSoundBuffer_Release(pBuf) */
void sub_00282F7E(void)
{
    uint32_t b = ARG(0), refs = 0;
    int id = voice_of(b);

    if (id && MEM32(b + OBJ_REFS)) {
        refs = (MEM32(b + OBJ_REFS) -= 1);
        if (!refs) {
            if (id < MAX_3D)
                src3d[id].used = false;
            mdr_voice_destroy(id);
            MEM32(b + OBJ_TAG) = 0;
            if (MEM32(b + OBJ_OWN))
                xbox_HeapFree(MEM32(b + OBJ_OWN));
            xbox_HeapFree(b);
        }
    }
    TRACE("Release 0x%08x (voice %d) -> %u", b, id, refs);
    RETURN(4, refs);
}

/* IDirectSoundBuffer_SetBufferData(pBuf, pvData, dwBytes): play the game's
 * own memory, which Lock then hands out. Bink creates its buffer with no
 * bytes, gives it its own memory here and Locks it to fill: Lock refused a
 * buffer that had memory only in the mixer, and the logo movies stalled
 * before their first frame. */
void sub_0028404A(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(12, DSERR_INVALIDCALL);
    mdr_voice_set_data(id, ARG(1), ARG(2));
    MEM32(ARG(0) + OBJ_DATA) = ARG(1);
    MEM32(ARG(0) + OBJ_BYTES) = ARG(2);
    TRACE("SetBufferData 0x%08x: 0x%08x, %u bytes", ARG(0), ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_Lock(pBuf, offset, bytes, ppv1, pdw1, ppv2, pdw2, flags) */
void sub_0028398F(void)
{
    uint32_t b = ARG(0), off = ARG(1), n = ARG(2), flags = ARG(7);
    uint32_t data = MEM32(b + OBJ_DATA), size = MEM32(b + OBJ_BYTES);
    uint32_t n1, n2;
    int id = voice_of(b);

    if (!id || !data || !size) {
        TRACE("Lock 0x%08x refused: voice %d, data 0x%08x, %u bytes", b, id, data, size);
        RETURN(32, DSERR_INVALIDCALL);
    }
    if (flags & 2)              /* DSBLOCK_ENTIREBUFFER */
        off = 0, n = size;
    else if (flags & 1)         /* DSBLOCK_FROMWRITECURSOR */
        off = mdr_voice_position(id);
    off %= size;
    if (n > size)
        n = size;
    n1 = size - off < n ? size - off : n;
    n2 = n - n1;
    TRACE("Lock 0x%08x offset %u, %u bytes, flags 0x%x", b, off, n, flags);
    if (ARG(3)) MEM32(ARG(3)) = data + off;
    if (ARG(4)) MEM32(ARG(4)) = n1;
    if (ARG(5)) MEM32(ARG(5)) = n2 ? data : 0;
    if (ARG(6)) MEM32(ARG(6)) = n2;
    RETURN(32, DS_OK);
}

/* IDirectSoundBuffer_Unlock(pBuf, pv1, dw1, pv2, dw2): the voice reads the
 * memory. */
void sub_00282F79(void)
{
    RETURN(20, DS_OK);
}

/* IDirectSoundBuffer_Play(pBuf, reserved1, reserved2, flags) */
void sub_0028400E(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(16, DSERR_INVALIDCALL);
    if (ARG(3) & 2)             /* DSBPLAY_FROMSTART */
        mdr_voice_set_position(id, 0);
    mdr_voice_play(id, (ARG(3) & 1) != 0);
    TRACE("Play 0x%08x flags 0x%x", ARG(0), ARG(3));
    RETURN(16, DS_OK);
}

/* IDirectSoundBuffer_Stop(pBuf) */
void sub_00284032(void)
{
    int id = voice_of(ARG(0));
    if (id)
        mdr_voice_stop(id);
    TRACE("Stop 0x%08x", ARG(0));
    RETURN(4, DS_OK);
}

/* IDirectSoundBuffer_GetStatus(pBuf, pdwStatus) */
void sub_00283937(void)
{
    int id = voice_of(ARG(0));
    bool looping = false, playing = id && mdr_voice_playing(id, &looping);
    if (ARG(1))
        MEM32(ARG(1)) = (playing ? 1u : 0u) | (playing && looping ? 4u : 0u);
    TRACE("GetStatus 0x%08x -> %s", ARG(0), playing ? "playing" : "stopped");
    RETURN(8, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_GetCurrentPosition(pBuf, pdwPlay, pdwWrite) */
void sub_00283953(void)
{
    int id = voice_of(ARG(0));
    uint32_t pos = id ? mdr_voice_position(id) : 0;
    TRACE("GetCurrentPosition 0x%08x -> %u", ARG(0), pos);
    if (ARG(1)) MEM32(ARG(1)) = pos;
    if (ARG(2)) MEM32(ARG(2)) = pos;
    RETURN(12, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_SetCurrentPosition(pBuf, dwOffset) */
void sub_00283973(void)
{
    int id = voice_of(ARG(0));
    if (id)
        mdr_voice_set_position(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetLoopRegion(pBuf, dwStart, dwLength) */
void sub_00283917(void)
{
    int id = voice_of(ARG(0));
    if (id)
        mdr_voice_set_loop_region(id, ARG(1), ARG(2));
    TRACE("SetLoopRegion 0x%08x %u, %u bytes", ARG(0), ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetVolume(pBuf, lVolume) */
void sub_002838AA(void)
{
    int id = voice_of(ARG(0));
    if (id)
        mdr_voice_set_volume(id, (int32_t)ARG(1));
    TRACE("SetVolume 0x%08x %d", ARG(0), (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetHeadroom(pBuf, dwHeadroom) */
void sub_002838C6(void)
{
    int id = voice_of(ARG(0));
    if (id)
        mdr_voice_set_headroom(id, ARG(1));
    TRACE("SetHeadroom 0x%08x %u", ARG(0), ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetFrequency(pBuf, dwFrequency) */
void sub_00283FD6(void)
{
    int id = voice_of(ARG(0));
    TRACE("SetFrequency 0x%08x %u Hz", ARG(0), ARG(1));
    if (id)
        mdr_voice_set_frequency(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetMixBins(pBuf, dwMixBinMask): 3936 takes a mask
 * (CDirectSoundVoice_SetMixBins 0x00283C2F), then resets the bins' volumes
 * to 0 dB; a 3D voice keeps its 3D bins. */
void sub_00283FF2(void)
{
    int id = voice_of(ARG(0));
    mdr_voice_bin bins[MDR_MAX_VOICE_BINS];
    int n;

    if (id) {
        bool positional = id < MAX_3D && src3d[id].used;
        n = bins_from_mask(ARG(1), positional, 2, bins);
        if (ARG(1) || positional)
            mdr_voice_set_bins(id, bins, n);
        TRACE("SetMixBins 0x%08x mask 0x%x, %d bins", ARG(0), ARG(1), n);
    }
    RETURN(8, DS_OK);
}

/* Per-buffer 3D state (see "3D sound" above). */
static int src_of(uint32_t buf)
{
    int id = voice_of(buf);
    return id > 0 && id < MAX_3D && src3d[id].used ? id : 0;
}

/* IDirectSoundBuffer_SetPosition(pBuf, x, y, z, apply) */
void sub_002838E2(void)
{
    int id = src_of(ARG(0));
    if (id) {
        for (int k = 0; k < 3; k++)
            src3d[id].pos[k] = fval(ARG(1 + k));
        TRACE("3D 0x%08x SetPosition %g %g %g apply %u", ARG(0), src3d[id].pos[0], src3d[id].pos[1],
              src3d[id].pos[2], ARG(4));
        apply_3d(id);
    }
    RETURN(20, DS_OK);
}
/* SetI3DL2Source(pBuf, pds3db, dwApply). DSI3DL2BUFFER: lDirect, lDirectHF,
 * lRoom, lRoomHF, flRoomRolloffFactor, Obstruction {lHFLevel, flLFRatio},
 * Occlusion {lHFLevel, flLFRatio}. */
static void trace_i3dl2_source(const char *what, uint32_t obj, uint32_t p)
{
    if (p)
        TRACE("%s 0x%08x direct %d hf %d room %d hf %d rolloff %.2f obstruction %d %.2f "
              "occlusion %d %.2f", what, obj, (int32_t)MEM32(p), (int32_t)MEM32(p + 4),
              (int32_t)MEM32(p + 8), (int32_t)MEM32(p + 12), fval(MEM32(p + 16)),
              (int32_t)MEM32(p + 20), fval(MEM32(p + 24)), (int32_t)MEM32(p + 28),
              fval(MEM32(p + 32)));
}
/* IDirectSoundBuffer_SetI3DL2Source */
void sub_002839BF(void)
{
    trace_i3dl2_source("buffer SetI3DL2Source", ARG(0), ARG(1));
    if (ARG(1))
        mdr_voice_set_i3dl2(voice_of(ARG(0)), (int32_t)MEM32(ARG(1)), (int32_t)MEM32(ARG(1) + 8));
    RETURN(12, DS_OK);
}

/* ── IDirectSoundStream ───────────────────────────────────────────────────
 * A stream is an XMediaObject: the game hands it packets with Process and
 * polls their status. Our stream objects carry the library's own vtable
 * (CDirectSoundStream, 0x002BE5A8), whose methods are replaced below, so
 * calls through the vtable and through the C wrappers both land here.
 *
 * Packets play back to back on a mixer voice (mdr_voice_queue). The
 * descriptor the game passes may be on its stack, so each packet's
 * pdwCompletedSize, pdwStatus and completion event are copied when it is
 * queued, and written from the mixer thread when it has played. */

#define TAG_STREAM          0x53524450u     /* "PDRS" */
#define STREAM_VTABLE       0x002BE5A8u   /* CDirectSoundStream (its constructor, 0x002842BC) */

#define STR_VTBL    0
#define STR_TAG     4
#define STR_VOICE   8
#define STR_REFS    12
#define STR_INDEX   16
#define STR_SIZE    32

#define XMEDIAPACKET_STATUS_SUCCESS 0x00000000u
#define XMEDIAPACKET_STATUS_PENDING 0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004u
#define XMO_STATUSF_ACCEPT_INPUT_DATA 0x00000001u

#define MAX_STREAMS     32
#define STREAM_PACKETS  64

typedef struct stream_packet {
    uint32_t completed_size, status, event, bytes;
} stream_packet;

typedef struct stream {
    uint32_t      obj;
    int           voice;
    uint32_t      max_packets;
    uint32_t      callback, context;
    uint32_t      block_align;
    uint32_t      next;                 /* next packet slot */
    stream_packet packets[STREAM_PACKETS];
} stream;

static stream streams[MAX_STREAMS];

static stream *stream_of(uint32_t obj)
{
    uint32_t i;

    if (!obj || MEM32(obj + STR_TAG) != TAG_STREAM)
        return NULL;
    i = MEM32(obj + STR_INDEX);
    return i < MAX_STREAMS && streams[i].obj == obj ? &streams[i] : NULL;
}

/* Mixer thread: a packet has been played, or flushed. */
static void packet_done(void *user, uint32_t tag, bool flushed)
{
    stream *s = user;
    stream_packet *p = &s->packets[tag % STREAM_PACKETS];

    if (p->completed_size)
        MEM32(p->completed_size) = flushed ? 0 : p->bytes;
    if (p->status)
        MEM32(p->status) = flushed ? XMEDIAPACKET_STATUS_FLUSHED : XMEDIAPACKET_STATUS_SUCCESS;
}

/* DirectSoundCreateStream(pdssd, ppStream). DSSTREAMDESC: dwFlags,
 * dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext,
 * dwMixBinMask (3936; a DSMIXBINS pointer in 4928). */
void sub_0028470F(void)
{
    uint32_t desc = ARG(0), pp = ARG(1), wfx, mixbins, obj;
    mdr_voice_desc vd;
    stream *s = NULL;
    int i;

    if (!desc || !pp)
        RETURN(8, DSERR_INVALIDCALL);
    for (i = 0; i < MAX_STREAMS && !s; i++)
        if (!streams[i].obj)
            s = &streams[i];
    if (!s)
        RETURN(8, DSERR_OUTOFMEMORY);
    if (!the_dsound)
        mdr_mixer_init();

    wfx = MEM32(desc + 8);
    mixbins = MEM32(desc + 20);
    memset(&vd, 0, sizeof vd);
    vd.format = MDR_FMT_PCM16;
    vd.channels = 2;
    vd.sample_rate = 48000;
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 1 ? 1 : 2;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = MDR_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? MDR_FMT_PCM8 : MDR_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: stream format 0x%04x, treating as PCM", tag);
        }
    }
    vd.bin_count = bins_from_mask(mixbins, (MEM32(desc) & 0x10) != 0, vd.channels, vd.bins);
    if (MEM32(desc) & 0x10)
        note_once("3D streams");

    memset(s, 0, sizeof *s);
    s->voice = mdr_voice_create(&vd);
    obj = guest_alloc(STR_SIZE);
    if (!s->voice || !obj)
        RETURN(8, DSERR_OUTOFMEMORY);
    s->obj = obj;
    s->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (s->max_packets > STREAM_PACKETS)
        s->max_packets = STREAM_PACKETS;
    s->callback = MEM32(desc + 12);
    s->context = MEM32(desc + 16);
    s->block_align = mdr_voice_block_align(s->voice);
    MEM32(obj + STR_VTBL) = STREAM_VTABLE;
    MEM32(obj + STR_TAG) = TAG_STREAM;
    MEM32(obj + STR_VOICE) = (uint32_t)s->voice;
    MEM32(obj + STR_REFS) = 1;
    MEM32(obj + STR_INDEX) = (uint32_t)(s - streams);
    mdr_voice_set_packet_done(s->voice, packet_done, s);
    mdr_voice_play(s->voice, false);    /* plays packets as they arrive */
    if (s->callback)
        note_once("stream packet callbacks");
    MEM32(pp) = obj;
    SDL_Log("dsound: stream 0x%08x: %s %u ch %u Hz, %u packets", obj,
            vd.format == MDR_FMT_XADPCM ? "adpcm" : vd.format == MDR_FMT_PCM8 ? "pcm8" : "pcm16",
            vd.channels, vd.sample_rate, s->max_packets);
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::AddRef(this) */
void sub_00283F52(void)
{
    uint32_t o = ARG(0);
    RETURN(4, stream_of(o) ? (MEM32(o + STR_REFS) += 1) : 0);
}

/* CDirectSoundStream::Release(this) */
void sub_00283F62(void)
{
    uint32_t o = ARG(0), refs = 0;
    stream *s = stream_of(o);

    if (s && MEM32(o + STR_REFS)) {
        refs = (MEM32(o + STR_REFS) -= 1);
        if (!refs) {
            mdr_voice_flush(s->voice);
            mdr_voice_destroy(s->voice);
            MEM32(o + STR_TAG) = 0;
            s->obj = 0;
            xbox_HeapFree(o);
        }
    }
    RETURN(4, refs);
}

/* CDirectSoundStream::GetInfo(this, pInfo): XMEDIAINFO { dwFlags,
 * dwInputSize, dwOutputSize, dwMaxLookahead }. */
void sub_002835ED(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t info = ARG(1);

    if (!s || !info)
        RETURN(8, DSERR_INVALIDCALL);
    MEM32(info + 0) = 0x00000005u;      /* XMO_STREAMF_FIXED_SAMPLE_SIZE | INPUT_ASYNC */
    MEM32(info + 4) = s->block_align;
    MEM32(info + 8) = 0;
    MEM32(info + 12) = 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::GetStatus(this, pdwStatus) */
void sub_00283692(void)
{
    stream *s = stream_of(ARG(0));

    if (!s)
        RETURN(8, DSERR_INVALIDCALL);
    if (ARG(1))
        MEM32(ARG(1)) = (uint32_t)mdr_voice_queued(s->voice) < s->max_packets
                      ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::Process(this, pInputPacket, pOutputPacket).
 * XMEDIAPACKET: pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
 * hCompletionEvent (pContext with a callback), prtTimestamp. */
void sub_002836C7(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t in = ARG(1), slot;
    stream_packet *p;

    if (!s || !in)
        RETURN(12, DSERR_INVALIDCALL);
    if ((uint32_t)mdr_voice_queued(s->voice) >= s->max_packets)
        RETURN(12, DSERR_INVALIDCALL);
    slot = s->next++ % STREAM_PACKETS;
    p = &s->packets[slot];
    p->bytes = MEM32(in + 4);
    p->completed_size = MEM32(in + 8);
    p->status = MEM32(in + 12);
    p->event = MEM32(in + 16);
    if (p->event && !s->callback)
        note_once("stream packet completion events");
    if (p->completed_size)
        MEM32(p->completed_size) = 0;
    if (p->status)
        MEM32(p->status) = XMEDIAPACKET_STATUS_PENDING;
    if (!mdr_voice_queue(s->voice, MEM32(in), p->bytes, slot)) {
        if (p->status)
            MEM32(p->status) = XMEDIAPACKET_STATUS_FLUSHED;
        RETURN(12, DSERR_INVALIDCALL);
    }
    TRACE("stream 0x%08x Process 0x%08x, %u bytes", ARG(0), MEM32(in), p->bytes);
    RETURN(12, DS_OK);
}

/* CDirectSoundStream::Discontinuity(this): the end of the data for now;
 * what is queued still plays. */
void sub_00283638(void)
{
    RETURN(4, stream_of(ARG(0)) ? DS_OK : DSERR_INVALIDCALL);
}

/* CDirectSoundStream::Flush(this) */
void sub_00283665(void)
{
    stream *s = stream_of(ARG(0));
    if (s)
        mdr_voice_flush(s->voice);
    RETURN(4, s ? DS_OK : DSERR_INVALIDCALL);
}

static int stream_voice(uint32_t obj)
{
    stream *s = stream_of(obj);
    return s ? s->voice : 0;
}

/* IDirectSoundStream_* setters, same arguments as the buffer ones. */
/* CDirectSoundStream_SetVolume(pStream, lVolume): `ret 8` */
void sub_002839DF(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        mdr_voice_set_volume(id, (int32_t)ARG(1));
    TRACE("stream SetVolume 0x%08x %d", ARG(0), (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundStream_SetPosition(pStream, x, y, z, apply): `ret 0x14`;
 * 3D streams are recorded only for now. */
void sub_002839F3(void) { note_once("3D streams (SetPosition)"); RETURN(20, DS_OK); }
/* IDirectSoundStream_SetI3DL2Source(pStream, pds3db, dwApply): a jump to
 * CDirectSoundStream_SetI3DL2Source (0x00283813, `ret 0xc`). */
void sub_00283A1C(void)
{
    trace_i3dl2_source("stream SetI3DL2Source", ARG(0), ARG(1));
    if (ARG(1) && stream_of(ARG(0)))
        mdr_voice_set_i3dl2(stream_of(ARG(0))->voice, (int32_t)MEM32(ARG(1)),
                            (int32_t)MEM32(ARG(1) + 8));
    RETURN(12, DS_OK);
}
