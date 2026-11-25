#ifndef MDR_CONFIG_H
#define MDR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define MDR_PATH_MAX 1024

/* VSync: On waits for the display (FIFO); Fast shows the newest frame at
 * each refresh without tearing (mailbox); Off shows frames at once (may
 * tear). */
enum { MDR_VSYNC_ON, MDR_VSYNC_FAST, MDR_VSYNC_OFF };

typedef struct mdr_config {
    char game_dir[MDR_PATH_MAX];    /* folder holding default.xbe and media/ */
} mdr_config;

/* Options the overlay menu (F1) changes; saved with the rest. */
typedef struct mdr_settings {
    int  render_scale2;     /* 2-16: render targets at half this multiple of the game's size (3: 1.5x) */
    int  msaa;              /* 1, 2, 4 or 8 samples */
    int  aspect;            /* MDR_ASPECT_*; 16:9 is the game's own widescreen mode */
    bool hud_edges;         /* 21:9: the HUD out to the screen's sides, not its 16:9 layout (d3d_hle.c) */
    int  fit;               /* MDR_FIT_*: how the picture fills the window */
    int  movie_fit;         /* MDR_MOVIE_*: how a movie's picture fills it (renderer.c) */
    bool soft_filter;       /* TV-like vertical softening at presentation */
    int  master_volume;     /* 0-100 % */
    bool show_fps;
    bool gpu_vertex;        /* vertex programs on the GPU (shaders/vsh.vert), not the CPU */
    bool gpu_fixed;         /* fixed-function vertices on the GPU (shaders/fvf.vert) */
    bool frame_interp;      /* frames between the game's 60, blended (renderer.c) */
    int  lsfg_multiplier;   /* Lossless Scaling frame generation: pictures per game
                               frame, 2-4; 1 off (lsfg_present.cpp, from the next start) */
    bool lsfg_performance;  /* its performance mode, lighter and less exact */
    char lsfg_dll[MDR_PATH_MAX];    /* the player's Lossless.dll; "" looks in the usual places */
    bool fullscreen;        /* borderless, the whole monitor (Alt+Enter) */
    bool fxaa;              /* edge smoothing on the finished frame (shaders/screen.frag) */
    int  vsync;             /* MDR_VSYNC_*: how frames reach the screen (renderer.c) */
    bool fps30;             /* Orta's frames shown every other one, a steady 30 fps (renderer.c) */
    bool shadow_original;   /* Shadows: Low, shadow maps at the game's size, not scaled (renderer.c) */
    int  anisotropy;        /* 1 off, 2, 4, 8, 16: anisotropic filtering of smooth-filtered textures */
    int  bloom;             /* 0 off, 1 low, 2 high: on the scene, before the HUD (renderer.c) */
    int  win_x, win_y, win_w, win_h;    /* the window when not fullscreen (w 0: not set yet) */
    bool win_maximized;
    bool mouse_aim;         /* the mouse steers the left stick (input_hle.c) */
    int  mouse_speed;       /* 10-400 % */
    bool unlock_box;        /* every Pandora's Box item open, the save untouched (src/guest/unlocks.c) */
    bool skip_logos;        /* a press during the boot logos goes to the title (src/guest/intro_skip.c) */
    bool skip_launcher;     /* start the game without the launcher (src/main.c; Shift held shows it) */
    int  rumble;            /* controller vibration strength, 0-100 % (src/hle/input_hle.c) */
    int  touch_buttons;     /* Android: MDR_TOUCH_*, when the on-screen controls show (src/android/) */
    int  touch_size;        /* Android: their size, 90-130 % (touch_controls.c) */
    int  touch_opacity;     /* Android: their opacity, 10-100 % */
    char gpu_driver[64];    /* Android: a custom Vulkan driver's folder in gpu_drivers/, "" the
                               system's (src/android/android_gpu.c, from the next start) */
    bool depth_reversed;    /* depth stored as 1 - z (renderer.c, read at start) */
    /* Picture (present.frag), on the finished frame: */
    bool picture_crt;       /* a CRT's darker midtones: x^(2.5/2.2) */
    int  pic_brightness;    /* -50..50, % added */
    int  pic_contrast;      /* 50..150 % */
    int  pic_gamma;         /* 50..200: 100 as is, more brightens the midtones */
    int  pic_saturation;    /* 0..200 % */
    int  audio_out;         /* MDR_AUDIO_*: stereo; headphones (with hrtf off, a model head);
                               or 5.1, 3D sounds around four speakers (mixer.c) */
    bool hrtf;              /* 3D sounds through the Xbox's HRTF filters (on 5.1 shared front and
                               back), else a pan, the model head or the corners (dsound_hle.c) */
} mdr_settings;

/* 21:9 is the game's widescreen mode made wider (2.37:1, 64:27). */
enum { MDR_ASPECT_4_3, MDR_ASPECT_16_9, MDR_ASPECT_21_9, MDR_ASPECT_COUNT };
enum { MDR_AUDIO_STEREO, MDR_AUDIO_HEADPHONES, MDR_AUDIO_SURROUND, MDR_AUDIO_COUNT };
/* Android's on-screen controls: hidden while a gamepad is connected, always shown, or never. */
enum { MDR_TOUCH_AUTO, MDR_TOUCH_ALWAYS, MDR_TOUCH_OFF, MDR_TOUCH_COUNT };
/* The whole picture with bars; stretched to the window; zoomed to fill it,
 * the overflow cut off. */
enum { MDR_FIT_BARS, MDR_FIT_STRETCH, MDR_FIT_ZOOM, MDR_FIT_COUNT };
/* Movies: as the game frames them (Fit to window, the default); or the
 * picture alone, whole (bars where the window's shape differs), stretched
 * or zoomed. */
enum { MDR_MOVIE_ORIGINAL, MDR_MOVIE_BARS, MDR_MOVIE_STRETCH, MDR_MOVIE_ZOOM, MDR_MOVIE_COUNT };

/* Widescreen (src/guest/widescreen.c): whether the game's cameras are wide
 * now (a race on screen, the Aspect setting wider than 4:3), and the shape
 * the picture is meant to be shown at (4/3 when not); frame_ratio: the shape
 * of the frame just finished (renderer.c, hand_over). */
bool mdr_widescreen_active(void);
float mdr_widescreen_ratio(void);
float mdr_widescreen_frame_ratio(void);
/* Always false now: the Aspect setting takes effect at once (kept for the
 * menu's help text). */
bool mdr_widescreen_needs_restart(void);
/* Game thread, at Present: whether a race is on screen (d3d_hle.c). */
void mdr_widescreen_race(bool race);

/* The current settings: defaults until mdr_config_load reads the file. */
mdr_settings *mdr_settings_get(void);
/* Write the settings, keeping the game folder the file already holds. */
bool mdr_settings_save(void);
/* A setting from the command line (--name value): name as in the settings
 * file (dashes for underscores; a few aliases: scale, output, volume...),
 * value as there. For this run only: a save keeps the file's value unless
 * the setting was changed since. False: no such setting. */
bool mdr_settings_set(const mdr_config *cfg, const char *name, const char *value);
/* Whether mdr_settings_set takes name (before the settings are loaded). */
bool mdr_settings_known(const char *name);
/* The settings into the log, when they differ from the last time. */
void mdr_settings_log(void);

/* Where settings, saves and imported game files live. With a portable.txt
 * next to the executable this is the executable's folder; a new player
 * gets one made there (portable by default); a player with data in the
 * per-user folder keeps it. Always ends in a path separator. */
const char *mdr_data_dir(void);
/* Which of those mdr_data_dir chose and why, for the log. */
extern const char *mdr_data_dir_note;

/* The log file this run writes, empty without one (main.c); a crash report
 * names it. */
extern char mdr_log_file[MDR_PATH_MAX];

/* The console's E: (TDATA, UDATA), where the game keeps its saves (several:
 * it has its own save-file screen): saves/E in the data folder. Written
 * without a trailing separator. */
void mdr_save_dir(char *out, size_t len);

bool mdr_config_load(mdr_config *cfg);
bool mdr_config_save(const mdr_config *cfg);

#endif
