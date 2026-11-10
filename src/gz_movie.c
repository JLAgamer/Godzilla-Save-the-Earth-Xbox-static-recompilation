/*
 * gz_movie.c - play the game's XMV movies on the host.
 *
 * The title's own XMV decoder never consumes its read buffers in the recomp,
 * so the movie texture stays black. FFmpeg reads XMV (WMV2 video, PCM audio),
 * so instead: when the game opens a movie, start "ffmpeg -i <file> -f rawvideo
 * -pix_fmt yuyv422 -s 640x480 -r 30 -" with its output on a pipe and extract
 * the audio to a temporary WAV played with PlaySound. Each time the game
 * updates the movie, the frame due by the wall clock is copied into the
 * movie's YUY2 texture, which the game draws as usual.
 *
 * ffmpeg.exe is looked up in GZ_FFMPEG, then next to the executable, then on
 * PATH. Without it (or with GZ_MOVIES=0) the caller falls back to skipping.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MOVIE_W 640
#define MOVIE_H 480
#define FRAME_BYTES (MOVIE_W * MOVIE_H * 2)
#define MOVIE_FPS 30

BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);

static struct {
    uint32_t obj;               /* guest movie object, 0 = idle */
    HANDLE   proc, pipe;
    DWORD    t0, last_update;
    int      frames_read, eof, have_frame;
    uint8_t *frame;
    char     wav[MAX_PATH];
} M;

static char s_ffmpeg[MAX_PATH];
static int  s_enabled = -1;

int gz_movie_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("GZ_MOVIES"), *f = getenv("GZ_FFMPEG");
        s_enabled = 0;
        if (e && *e == '0') {
            fprintf(stderr, "[MOVIE] GZ_MOVIES=0: movies are skipped\n");
        } else if (f && *f && GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) {
            strncpy(s_ffmpeg, f, sizeof s_ffmpeg - 1);
            s_enabled = 1;
        } else if (SearchPathA(NULL, "ffmpeg.exe", NULL, sizeof s_ffmpeg, s_ffmpeg, NULL)) {
            s_enabled = 1;
        } else {
            fprintf(stderr, "[MOVIE] ffmpeg.exe not found (GZ_FFMPEG, exe folder, PATH): movies are skipped\n");
        }
        if (s_enabled) fprintf(stderr, "[MOVIE] host playback through %s\n", s_ffmpeg);
    }
    return s_enabled;
}

void gz_movie_close(void)
{
    if (!M.obj) return;
    PlaySoundA(NULL, NULL, 0);
    if (M.proc) { TerminateProcess(M.proc, 0); CloseHandle(M.proc); }
    if (M.pipe) CloseHandle(M.pipe);
    if (M.wav[0]) DeleteFileA(M.wav);
    free(M.frame);
    memset(&M, 0, sizeof M);
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

/* The game opened a movie. Returns 1 if the host will play it. */
int gz_movie_open(uint32_t obj, const char *xbox_path)
{
    WCHAR wpath[MAX_PATH];
    char path[MAX_PATH], cmd[2048], tmp[MAX_PATH];
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

    gz_movie_close();
    if (!gz_movie_enabled() || !xbox_path) return 0;
    if (!xbox_translate_path(xbox_path, wpath, MAX_PATH)
            || !WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, sizeof path, NULL, NULL)
            || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[MOVIE] %s: no host file\n", xbox_path);
        return 0;
    }
    if (!CreatePipe(&rd, &wr, &sa, FRAME_BYTES * 4)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    snprintf(cmd, sizeof cmd,
             "\"%s\" -v quiet -i \"%s\" -an -f rawvideo -pix_fmt yuyv422 -s %dx%d -r %d -",
             s_ffmpeg, path, MOVIE_W, MOVIE_H, MOVIE_FPS);
    if (!run(cmd, wr, &M.proc, 0)) {
        CloseHandle(rd); CloseHandle(wr);
        fprintf(stderr, "[MOVIE] could not start ffmpeg\n");
        return 0;
    }
    CloseHandle(wr);
    M.pipe = rd;
    M.obj = obj;
    M.frame = (uint8_t *)malloc(FRAME_BYTES);

    /* Audio: PCM in the file; copy it to a WAV and let Windows play it. */
    GetTempPathA(sizeof tmp, tmp);
    snprintf(M.wav, sizeof M.wav, "%sgz_movie_%lu.wav", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(cmd, sizeof cmd, "\"%s\" -v quiet -y -i \"%s\" -vn -c:a pcm_s16le \"%s\"", s_ffmpeg, path, M.wav);
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        int have = run(cmd, NULL, NULL, 1) && GetFileAttributesExA(M.wav, GetFileExInfoStandard, &fa);
        BOOL ok = have ? PlaySoundA(M.wav, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) : FALSE;
        fprintf(stderr, "[MOVIE] audio: %s (%lu KB WAV), playback %s\n",
                have ? "extracted" : "none", have ? (unsigned long)(fa.nFileSizeLow / 1024) : 0ul,
                ok ? "started" : "not started");
    }
    M.t0 = M.last_update = GetTickCount();
    fprintf(stderr, "[MOVIE] playing %s on the host\n", xbox_path);
    return 1;
}

static int read_frame(void)
{
    DWORD got = 0, n;
    while (got < FRAME_BYTES) {
        if (!ReadFile(M.pipe, M.frame + got, FRAME_BYTES - got, &n, NULL) || n == 0) return 0;
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
    M.last_update = GetTickCount();
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
    if (M.obj && GetTickCount() - M.last_update > 700) {
        fprintf(stderr, "[MOVIE] abandoned by the game after %d frames\n", M.frames_read);
        gz_movie_close();
    }
}
