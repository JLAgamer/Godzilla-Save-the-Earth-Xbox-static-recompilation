/*
 * gz_movie.c - play the game's XMV movies on the host.
 *
 * The title's own XMV decoder never consumes its read buffers in the recomp,
 * so the movie texture stays black. FFmpeg reads XMV (WMV2 video, PCM audio),
 * so instead: when the game opens a movie, start "ffmpeg -i <file> -f rawvideo
 * -pix_fmt yuyv422 -s 640x480 -r 30 -" with its output on a pipe and extract
 * the audio to a temporary WAV, played with PlaySound on Windows and through
 * an SDL2 audio device on Linux. Each time the game updates the movie, the
 * frame due by the wall clock is copied into the movie's YUY2 texture, which
 * the game draws as usual.
 *
 * ffmpeg (ffmpeg.exe on Windows) is looked up in GZ_FFMPEG, then next to the
 * executable, then on PATH. Without it (or with GZ_MOVIES=0) the caller falls
 * back to skipping.
 */
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <SDL.h>
#include "platform/xbox_winnt.h"   /* the toolkit's POSIX BOOL, DWORD */
#include "gz_host.h"               /* gz_exe_dir */
extern char **environ;
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MOVIE_W 640
#define MOVIE_H 480
#define FRAME_BYTES (MOVIE_W * MOVIE_H * 2)
#define MOVIE_FPS 30
#define GZ_MOVIE_PATH 1024

#ifdef _WIN32
BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);
#else
BOOL xbox_translate_path(const char *xbox_path, char *host_path_buf, DWORD buf_size);
#endif

static struct {
    uint32_t obj;               /* guest movie object, 0 = idle */
#ifdef _WIN32
    HANDLE   proc, pipe;
#else
    pid_t    pid;
    int      fd;                /* read end of ffmpeg's stdout, -1 = none */
    SDL_AudioDeviceID dev;      /* the soundtrack's device, 0 = none */
#endif
    uint32_t t0, last_update;
    int      frames_read, eof, have_frame;
    uint8_t *frame;
    char     wav[GZ_MOVIE_PATH];
} M;

static char s_ffmpeg[GZ_MOVIE_PATH];
static int  s_enabled = -1;

/* ── Host primitives ── */

#ifdef _WIN32
static uint32_t now_ms(void) { return GetTickCount(); }

static int file_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

/* ffmpeg.exe in the exe folder or on PATH (SearchPath's order). */
static int find_ffmpeg(void)
{
    return SearchPathA(NULL, "ffmpeg.exe", NULL, sizeof s_ffmpeg, s_ffmpeg, NULL) != 0;
}

static int run(const char *cmd, HANDLE out, HANDLE *proc, int wait)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[2048];
    HANDLE nul;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdError = nul; si.hStdOutput = out ? out : nul;
    strncpy(buf, cmd, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(nul);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(nul);
    if (wait) { WaitForSingleObject(pi.hProcess, 20000); CloseHandle(pi.hProcess); }
    else *proc = pi.hProcess;
    return 1;
}

/* The movie file as a host path. */
static int host_path(const char *xbox_path, char *path, int n)
{
    WCHAR wpath[MAX_PATH];
    return xbox_translate_path(xbox_path, wpath, MAX_PATH)
        && WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, n, NULL, NULL)
        && file_exists(path);
}

/* Video: ffmpeg's raw frames on a pipe. */
static int start_video(const char *path)
{
    char cmd[2048];
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

    if (!CreatePipe(&rd, &wr, &sa, FRAME_BYTES * 4)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    snprintf(cmd, sizeof cmd,
             "\"%s\" -v quiet -i \"%s\" -an -f rawvideo -pix_fmt yuyv422 -s %dx%d -r %d -",
             s_ffmpeg, path, MOVIE_W, MOVIE_H, MOVIE_FPS);
    if (!run(cmd, wr, &M.proc, 0)) {
        CloseHandle(rd); CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);
    M.pipe = rd;
    return 1;
}

/* Audio: PCM in the file; copy it to a WAV and let Windows play it. */
static void start_audio(const char *path)
{
    char cmd[2048], tmp[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int have;
    BOOL ok;

    GetTempPathA(sizeof tmp, tmp);
    snprintf(M.wav, sizeof M.wav, "%sgz_movie_%lu.wav", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(cmd, sizeof cmd, "\"%s\" -v quiet -y -i \"%s\" -vn -c:a pcm_s16le \"%s\"", s_ffmpeg, path, M.wav);
    have = run(cmd, NULL, NULL, 1) && GetFileAttributesExA(M.wav, GetFileExInfoStandard, &fa);
    ok = have ? PlaySoundA(M.wav, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) : FALSE;
    fprintf(stderr, "[MOVIE] audio: %s (%lu KB WAV), playback %s\n",
            have ? "extracted" : "none", have ? (unsigned long)(fa.nFileSizeLow / 1024) : 0ul,
            ok ? "started" : "not started");
}

static void stop_host(void)
{
    PlaySoundA(NULL, NULL, 0);
    if (M.proc) { TerminateProcess(M.proc, 0); CloseHandle(M.proc); }
    if (M.pipe) CloseHandle(M.pipe);
    if (M.wav[0]) DeleteFileA(M.wav);
}

static int read_bytes(uint8_t *dst, int want)
{
    DWORD n;
    if (!ReadFile(M.pipe, dst, (DWORD)want, &n, NULL)) return 0;
    return (int)n;
}

#else /* POSIX */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_exec(const char *p) { return file_exists(p) && access(p, X_OK) == 0; }

/* ffmpeg next to the program, else on PATH (Windows' SearchPath order). */
static int find_ffmpeg(void)
{
    char dir[GZ_MOVIE_PATH];
    const char *path = getenv("PATH");

    gz_exe_dir(dir, sizeof dir);
    if (dir[0]) {
        snprintf(s_ffmpeg, sizeof s_ffmpeg, "%s/ffmpeg", dir);
        if (is_exec(s_ffmpeg)) return 1;
    }
    while (path && *path) {
        const char *end = strchr(path, ':');
        int len = end ? (int)(end - path) : (int)strlen(path);
        if (len > 0 && len < (int)sizeof dir - 8) {
            snprintf(s_ffmpeg, sizeof s_ffmpeg, "%.*s/ffmpeg", len, path);
            if (is_exec(s_ffmpeg)) return 1;
        }
        path = end ? end + 1 : NULL;
    }
    s_ffmpeg[0] = 0;
    return 0;
}

/* Starts argv with stdin and stderr on /dev/null and stdout on out_fd (-1:
 * /dev/null). wait: until it exits (up to 20 s, like Windows), returning its
 * success; else *pid gets the child. */
static int run(char *const argv[], int out_fd, pid_t *pid, int wait)
{
    posix_spawn_file_actions_t fa;
    pid_t child;
    int rc;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (out_fd >= 0) posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    rc = posix_spawn(&child, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 0;
    if (!wait) { *pid = child; return 1; }
    {
        int st = 0, i;
        for (i = 0; i < 2000; i++) {          /* 20 s */
            pid_t r = waitpid(child, &st, WNOHANG);
            if (r == child) return WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (r < 0 && errno != EINTR) return 0;
            usleep(10000);
        }
        kill(child, SIGKILL);
        waitpid(child, &st, 0);
        return 0;
    }
}

/* The movie file as a host path (the kernel resolves case on POSIX). */
static int host_path(const char *xbox_path, char *path, int n)
{
    return xbox_translate_path(xbox_path, path, (DWORD)n) && file_exists(path);
}

static int start_video(const char *path)
{
    char size[32], rate[16];
    char *argv[] = { s_ffmpeg, "-v", "quiet", "-i", (char *)path, "-an", "-f", "rawvideo",
                     "-pix_fmt", "yuyv422", "-s", size, "-r", rate, "-", NULL };
    int fds[2];

    snprintf(size, sizeof size, "%dx%d", MOVIE_W, MOVIE_H);
    snprintf(rate, sizeof rate, "%d", MOVIE_FPS);
    if (pipe2(fds, O_CLOEXEC) != 0) return 0;
    fcntl(fds[0], F_SETPIPE_SZ, FRAME_BYTES * 4);   /* best effort, like CreatePipe's size */
    if (!run(argv, fds[1], &M.pid, 0)) {
        close(fds[0]); close(fds[1]);
        return 0;
    }
    close(fds[1]);
    M.fd = fds[0];
    return 1;
}

/* Audio: extract the PCM to a WAV and queue it on its own SDL2 device, next
 * to the one the game's mixer (the toolkit's apu_sdl2.c) uses. */
static void start_audio(const char *path)
{
    char *argv[] = { s_ffmpeg, "-v", "quiet", "-y", "-i", (char *)path, "-vn",
                     "-c:a", "pcm_s16le", M.wav, NULL };
    const char *tmp = getenv("TMPDIR");
    SDL_AudioSpec spec, have;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    int ok = 0, got;

    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(M.wav, sizeof M.wav, "%s/gz_movie_%lu.wav", tmp, (unsigned long)getpid());
    got = run(argv, -1, NULL, 1) && file_exists(M.wav);
    if (got && (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
            && SDL_LoadWAV(M.wav, &spec, &buf, &len)) {
        spec.callback = NULL;                                    /* push: SDL_QueueAudio */
        spec.userdata = NULL;
        M.dev = SDL_OpenAudioDevice(NULL, 0, &spec, &have, 0);   /* SDL converts if it must */
        if (M.dev && SDL_QueueAudio(M.dev, buf, len) == 0) {
            SDL_PauseAudioDevice(M.dev, 0);
            ok = 1;
        } else if (M.dev) {
            SDL_CloseAudioDevice(M.dev);
            M.dev = 0;
        }
        SDL_FreeWAV(buf);
    }
    fprintf(stderr, "[MOVIE] audio: %s (%lu KB WAV), playback %s%s%s\n",
            got ? "extracted" : "none", (unsigned long)(len / 1024),
            ok ? "started" : "not started", ok || !got ? "" : ": ", ok || !got ? "" : SDL_GetError());
}

static void stop_host(void)
{
    if (M.dev) SDL_CloseAudioDevice(M.dev);
    if (M.pid > 0) { kill(M.pid, SIGKILL); waitpid(M.pid, NULL, 0); }
    if (M.fd >= 0) close(M.fd);
    if (M.wav[0]) unlink(M.wav);
}

static int read_bytes(uint8_t *dst, int want)
{
    ssize_t n;
    do n = read(M.fd, dst, (size_t)want); while (n < 0 && errno == EINTR);
    return n > 0 ? (int)n : 0;
}
#endif

/* ── Shared ── */

static void reset(void)
{
    memset(&M, 0, sizeof M);
#ifndef _WIN32
    M.fd = -1;
#endif
}

int gz_movie_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("GZ_MOVIES"), *f = getenv("GZ_FFMPEG");
        reset();
        s_enabled = 0;
        if (e && *e == '0') {
            fprintf(stderr, "[MOVIE] GZ_MOVIES=0: movies are skipped\n");
        } else if (f && *f && file_exists(f)) {
            strncpy(s_ffmpeg, f, sizeof s_ffmpeg - 1);
            s_enabled = 1;
        } else if (find_ffmpeg()) {
            s_enabled = 1;
        } else {
#ifdef _WIN32
            fprintf(stderr, "[MOVIE] ffmpeg.exe not found (GZ_FFMPEG, exe folder, PATH): movies are skipped\n");
#else
            fprintf(stderr, "[MOVIE] ffmpeg not found (GZ_FFMPEG, exe folder, PATH): movies are skipped\n");
#endif
        }
        if (s_enabled) fprintf(stderr, "[MOVIE] host playback through %s\n", s_ffmpeg);
    }
    return s_enabled;
}

void gz_movie_close(void)
{
    if (!M.obj) return;
    stop_host();
    free(M.frame);
    reset();
}

/* The game opened a movie. Returns 1 if the host will play it. */
int gz_movie_open(uint32_t obj, const char *xbox_path)
{
    char path[GZ_MOVIE_PATH];

    gz_movie_close();
    if (!gz_movie_enabled() || !xbox_path) return 0;
    if (!host_path(xbox_path, path, sizeof path)) {
        fprintf(stderr, "[MOVIE] %s: no host file\n", xbox_path);
        return 0;
    }
    if (!start_video(path)) {
        fprintf(stderr, "[MOVIE] could not start ffmpeg\n");
        return 0;
    }
    M.obj = obj;
    M.frame = (uint8_t *)malloc(FRAME_BYTES);
    start_audio(path);
    M.t0 = M.last_update = now_ms();
    fprintf(stderr, "[MOVIE] playing %s on the host\n", xbox_path);
    return 1;
}

static int read_frame(void)
{
    int got = 0, n;
    while (got < FRAME_BYTES) {
        if ((n = read_bytes(M.frame + got, FRAME_BYTES - got)) <= 0) return 0;
        got += n;
    }
    return 1;
}

/* Per game update of movie `obj`: copy the frame due now into dst (YUY2,
 * 640x480, pitch 1280) when dst is not NULL. Returns 1 while playing, 2 at the
 * end of the movie, 0 if the host is not playing this object. */
int gz_movie_update(uint32_t obj, uint8_t *dst)
{
    int want;
    if (!M.obj || M.obj != obj) return 0;
    M.last_update = now_ms();
    want = (int)((uint64_t)(M.last_update - M.t0) * MOVIE_FPS / 1000u) + 1;
    while (!M.eof && M.frames_read < want) {
        if (read_frame()) { M.frames_read++; M.have_frame = 1; }
        else M.eof = 1;
    }
    if (dst && M.have_frame) memcpy(dst, M.frame, FRAME_BYTES);
    if (M.eof) {
        fprintf(stderr, "[MOVIE] finished after %d frames\n", M.frames_read);
        gz_movie_close();
        return 2;
    }
    return 1;
}

/* Once per game frame: a movie the game stopped updating (skipped with a
 * button, or its screen closed) must not keep its sound running. */
void gz_movie_tick(void)
{
    if (M.obj && now_ms() - M.last_update > 700) {
        fprintf(stderr, "[MOVIE] abandoned by the game after %d frames\n", M.frames_read);
        gz_movie_close();
    }
}
