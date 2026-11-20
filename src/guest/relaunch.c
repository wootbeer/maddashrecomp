/*
 * A title launching an image (from Orta Recomp, whose disc had two
 * programs). Mad Dash Racing has one, and launches it again to load every
 * level: the front end boots default.xbe with the race in its launch data,
 * and the race boots it again to go back. A launch of the dashboard or of
 * an image this build does not have exits.
 *
 * A title boots an image with XLaunchNewImage: it fills the kernel's launch
 * data page (type, title, path, then 3 KB of its own data) and asks
 * HalReturnToFirmware to reboot (routine 2). xboxrecomp's kernel calls
 * xbox_relaunch_hook there (patch 0037).
 *
 * The program running now, booted again: in place (mdr_guest_reboot, patch
 * 0047), keeping the window, the GPU device and the sound output. Memory
 * and the kernel layer start over as at start-up, the page is put back where
 * the title reads it (XGetLaunchInfo), as the console's reboot keeps it, and
 * the title's entry point runs again. The old boot's other threads park.
 *
 * Another program of this build (none in Mad Dash), or a reboot the kernel
 * layer cannot do in place: the page is saved to a file and MadDashRecomp
 * starts again on that program with --launch-data. The window closes and
 * opens again; settings, the window's place and the log carry on (the log
 * is appended to). On Android the activity starts again the same way
 * (MadDashActivity.restartWith).
 */

#include "guest/guest.h"
#include "guest/program.h"
#include "config.h"

#include "kernel.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

#define PAGE_SIZE   4096u
#define PATH_OFFSET 8u          /* szLaunchPath, after LaunchDataType and TitleId */

extern ptrdiff_t g_xbox_mem_offset;
extern void (*xbox_relaunch_hook)(ULONG Routine);

static int saved_argc;
static char **saved_argv;

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static uint32_t launch_page(void)
{
    uint32_t v;
    memcpy(&v, guest(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE), 4);
    return v;
}

static void launch_data_path(char *out, size_t n)
{
    snprintf(out, n, "%slaunch_data.bin", mdr_data_dir());
}

/* The program an image path names ("\Device\CdRom0;default.xbe", "d:\default.xbe"),
 * if this build has it. */
static bool program_of(const char *path, mdr_program *out)
{
    const char *base = path;

    for (const char *c = path; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ';' || *c == ':')
            base = c + 1;
    for (int p = 0; p < MDR_PROGRAM_COUNT; p++) {
        if (SDL_strcasecmp(base, mdr_program_xbe((mdr_program)p)) == 0
                && mdr_program_built_from((mdr_program)p)) {
            *out = (mdr_program)p;
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
/* One argument quoted for CommandLineToArgvW / the C runtime's parser. */
static void append_quoted(wchar_t *cmd, size_t cap, const char *arg)
{
    wchar_t w[1024];
    size_t n = wcslen(cmd);
    int len = MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, (int)SDL_arraysize(w));

    if (len <= 0 || n + 3 >= cap)
        return;
    if (n)
        cmd[n++] = L' ';
    cmd[n++] = L'"';
    for (const wchar_t *c = w; *c && n + 4 < cap; c++) {
        size_t slashes = 0;
        while (c[0] == L'\\') {
            slashes++;
            c++;
        }
        if (!*c || *c == L'"') {        /* backslashes before a quote or the end double */
            for (size_t i = 0; i < slashes * 2 && n + 4 < cap; i++)
                cmd[n++] = L'\\';
            if (!*c)
                break;
            cmd[n++] = L'\\';
            cmd[n++] = L'"';
        } else {
            for (size_t i = 0; i < slashes && n + 4 < cap; i++)
                cmd[n++] = L'\\';
            cmd[n++] = *c;
        }
    }
    cmd[n++] = L'"';
    cmd[n] = 0;
}
#endif

#ifdef _WIN32
typedef struct window_find { DWORD pid; HWND hwnd; } window_find;

static BOOL CALLBACK window_of_cb(HWND hwnd, LPARAM lp)
{
    window_find *wf = (window_find *)lp;
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == wf->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        wf->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* A visible top-level window of process `pid`, if it has one yet. */
static HWND window_of(DWORD pid)
{
    window_find wf = { pid, NULL };
    EnumWindows(window_of_cb, (LPARAM)&wf);
    return wf.hwnd;
}
#endif

/* Start MadDashRecomp again on `program`: this run's arguments without the ones
 * that chose the program or the launcher, then --boot --program NAME
 * --launch-data FILE. Returns only if it could not. */
static void restart_on(mdr_program program, const char *data_file)
{
    const char *extra[] = { "--boot", "--program", mdr_program_name(program),
                            "--launch-data", data_file };
    const char *args[64];
    int n = 0;

    args[n++] = saved_argv[0];
    for (int i = 1; i < saved_argc && n < 50; i++) {
        const char *a = saved_argv[i];
        if (!strcmp(a, "--program") || !strcmp(a, "--launch-data") || !strcmp(a, "--import-iso")) {
            i++;                        /* and its value */
            continue;
        }
        if (!strcmp(a, "--boot") || !strcmp(a, "--no-boot") || !strcmp(a, "--setup") || !strcmp(a, "--check"))
            continue;
        args[n++] = a;
    }
    for (size_t i = 0; i < SDL_arraysize(extra); i++)
        args[n++] = extra[i];
    args[n] = NULL;

    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    {
        static wchar_t exe[MAX_PATH], cmd[8192];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
            return;
        cmd[0] = 0;
        for (int i = 0; i < n; i++)
            append_quoted(cmd, SDL_arraysize(cmd), args[i]);
        SDL_zero(si);
        si.cb = sizeof si;
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            SDL_Log("launch: cannot start %s again (error %lu)", mdr_program_name(program), GetLastError());
            return;
        }
        /* Windows lets a process take the foreground only if the one in
         * front allows it: the new window otherwise opened without the
         * focus, and so without the gamepad, until clicked. Allowing it
         * and leaving at once was not enough: this window closed first,
         * Windows put the next one in line in front (a browser, say), and
         * the new window, seconds later, opened behind it. So this one
         * stays until the new process shows its window, puts that in front
         * while it still has the foreground itself, and only then goes;
         * at most 20 s, or as soon as the new process ends. */
        AllowSetForegroundWindow(pi.dwProcessId);
        {
            HWND found = NULL;
            for (int i = 0; i < 200 && !found; i++) {
                found = window_of(pi.dwProcessId);
                if (!found && WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
                    break;
            }
            if (found) {
                SetForegroundWindow(found);
                SDL_Log("launch: the new window is in front");
            } else {
                SDL_Log("launch: no window from the new process to put in front");
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#elif defined(__ANDROID__)
    {
        /* An app cannot exec itself: MadDashActivity.restartWith hands the
         * arguments (without argv[0]) to RelaunchActivity, in a process of
         * its own, which starts the game again once this process ends. */
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        jobject activity = (jobject)SDL_GetAndroidActivity();
        jclass cls = activity ? (*env)->GetObjectClass(env, activity) : NULL;
        jmethodID mid = cls ? (*env)->GetStaticMethodID(env, cls, "restartWith", "([Ljava/lang/String;)V") : NULL;
        jclass string_cls = (*env)->FindClass(env, "java/lang/String");
        jobjectArray jargs = mid && string_cls ? (*env)->NewObjectArray(env, n - 1, string_cls, NULL) : NULL;

        if (jargs) {
            for (int i = 1; i < n; i++) {
                jstring a = (*env)->NewStringUTF(env, args[i]);
                (*env)->SetObjectArrayElement(env, jargs, i - 1, a);
                (*env)->DeleteLocalRef(env, a);
            }
            (*env)->CallStaticVoidMethod(env, cls, mid, jargs);
        }
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        SDL_Log("launch: cannot start %s again", mdr_program_name(program));
    }
#else
    /* The same process, a new image: Steam and the Deck see no exit. */
    execv("/proc/self/exe", (char *const *)args);
    SDL_Log("launch: cannot start %s again: %s", mdr_program_name(program), strerror(errno));
#endif
}

static void relaunch_hook(ULONG routine)
{
    char path[64], file[MDR_PATH_MAX];
    uint32_t page = launch_page();
    mdr_program program;
    size_t i;
    SDL_IOStream *io;

    if (routine != 2 || !page)
        return;                         /* a plain reboot or quit: exit as before */
    for (i = 0; i + 1 < sizeof path && guest(page + PATH_OFFSET)[i]; i++)
        path[i] = (char)guest(page + PATH_OFFSET)[i];
    path[i] = 0;
    if (!path[0]) {
        SDL_Log("launch: %s returns to the dashboard; exiting", mdr_program_name(mdr_program_active()));
        return;
    }
    if (!program_of(path, &program)) {
        SDL_Log("launch: %s asks for %s, which this build cannot run; exiting",
                mdr_program_name(mdr_program_active()), path);
        return;
    }
    if (program == mdr_program_active()) {
        uint8_t data[PAGE_SIZE];
        memcpy(data, guest(page), PAGE_SIZE);
        SDL_Log("launch: %s boots %s again (%s); rebooting in place",
                mdr_program_name(program), mdr_program_xbe(program), path);
        mdr_guest_reboot(data, PAGE_SIZE);      /* returns only if it cannot */
        SDL_Log("launch: cannot reboot in place; restarting");
    }
    launch_data_path(file, sizeof file);
    io = SDL_IOFromFile(file, "wb");
    if (!io || SDL_WriteIO(io, guest(page), PAGE_SIZE) != PAGE_SIZE) {
        SDL_Log("launch: cannot write %s: %s", file, SDL_GetError());
        if (io)
            SDL_CloseIO(io);
        return;
    }
    SDL_CloseIO(io);
    SDL_Log("launch: %s starts %s (%s); restarting on it",
            mdr_program_name(mdr_program_active()), mdr_program_name(program), path);
    mdr_settings_save();                /* the window's place and size */
    restart_on(program, file);
}

void mdr_relaunch_install(int argc, char **argv)
{
    saved_argc = argc;
    saved_argv = argv;
    xbox_relaunch_hook = relaunch_hook;
}

bool mdr_relaunch_load(const char *file)
{
    size_t size = 0;
    void *data = SDL_LoadFile(file, &size);
    uint32_t page;

    if (!data || size != PAGE_SIZE) {
        SDL_Log("launch: no launch data in %s", file);
        SDL_free(data);
        return false;
    }
    page = xbox_HeapAlloc(PAGE_SIZE, PAGE_SIZE);
    if (page) {
        memcpy(guest(page), data, PAGE_SIZE);
        memcpy(guest(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE), &page, 4);
        SDL_Log("launch: launch data from the program before (type %u, from %08x)",
                ((uint32_t *)data)[0], ((uint32_t *)data)[1]);
    }
    SDL_free(data);
    SDL_RemovePath(file);
    return page != 0;
}
