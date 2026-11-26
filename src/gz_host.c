/*
 * gz_host.c - host services for the game-side files (gz_host.h).
 *
 * The Windows half is what gz_launch.c and main.c called directly before;
 * the POSIX half does the same with libc, for the Linux / Steam Deck build.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE        /* FNM_CASEFOLD */
#endif
#include "gz_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* ======================================================================== */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>

void gz_host_set_args(int argc, char **argv) { (void)argc; (void)argv; }

gzch **gz_host_args(int *argc)
{
    return CommandLineToArgvW(GetCommandLineW(), argc);
}

void gz_host_free_args(gzch **argv) { if (argv) LocalFree(argv); }

void gz_to_utf8(const gzch *w, char *out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0;
}

int gz_from_text(const char *line, gzch *out, int n)
{
    return MultiByteToWideChar(CP_OEMCP, 0, line, -1, out, n) != 0;
}

int gz_file_exists(const gzch *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int gz_dir_exists(const gzch *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int gz_path_exists(const gzch *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

void gz_make_dirs(const gzch *path)
{
    WCHAR tmp[MAX_PATH], *p;
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    for (p = tmp + 3; *p; p++)
        if (*p == L'\\' || *p == L'/') { WCHAR c = *p; *p = 0; CreateDirectoryW(tmp, NULL); *p = c; }
    CreateDirectoryW(tmp, NULL);
}

int gz_delete_file(const gzch *p) { return DeleteFileW(p) != 0; }
int gz_move_replace(const gzch *a, const gzch *b) { return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) != 0; }
int gz_copy_file_new(const gzch *a, const gzch *b) { return CopyFileW(a, b, TRUE) != 0; }

int gz_folder_writable(const gzch *dir)
{
    WCHAR p[MAX_PATH];
    HANDLE h;
    swprintf_s(p, MAX_PATH, L"%s\\gz_write_test.tmp", dir);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

int gz_full_path(const gzch *in, gzch *out, int n) { return GetFullPathNameW(in, (DWORD)n, out, NULL) != 0; }
int gz_chdir(const gzch *dir) { return SetCurrentDirectoryW(dir) != 0; }

void gz_file_info(const gzch *p, unsigned long long *size, unsigned long long *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    memset(&fa, 0, sizeof fa);
    GetFileAttributesExW(p, GetFileExInfoStandard, &fa);
    *size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    *mtime = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

void gz_list_dir(const gzch *dir, gz_dir_fn fn, void *ctx)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void gz_exe_dir(gzch *out, int n)
{
    WCHAR *slash;
    GetModuleFileNameW(NULL, out, (DWORD)n);
    if ((slash = wcsrchr(out, L'\\')) != NULL) *slash = 0;
}

int gz_user_data_dir(gzch *out, int n)
{
    DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", out, (DWORD)n);
    return k && k < (DWORD)n;
}

void gz_message_box(const gzch *text, const gzch *caption, int warning)
{
    MessageBoxW(NULL, text, caption, warning ? MB_ICONWARNING : MB_ICONINFORMATION);
}

void gz_sha1_hex(const void *data, size_t n, char out[41])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char d[20];
    int i;
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0) return;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) == 0 && BCryptFinishHash(h, d, 20, 0) == 0)
            for (i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", d[i]);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

void gz_list_dir_a(const char *dir, const char *pattern, gz_dir_fn_a fn, void *ctx)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\%s", dir, pattern);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        fn(fd.cFileName, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int gz_delete_file_a(const char *p) { return DeleteFileA(p) != 0; }

void gz_file_info_a(const char *p, unsigned long long *size, unsigned long long *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    memset(&fa, 0, sizeof fa);
    GetFileAttributesExA(p, GetFileExInfoStandard, &fa);
    *size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    *mtime = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

int gz_single_instance(const char *key)
{
    char name[MAX_PATH + 32], *c;
    HANDLE m;
    snprintf(name, sizeof name, "Local\\gz_cache_%s", key);
    for (c = name + 6; *c; c++) if (*c == '\\' || *c == ':' || *c == '/') *c = '_';
    m = CreateMutexA(NULL, FALSE, name);       /* kept open for the whole run */
    return !(m && GetLastError() == ERROR_ALREADY_EXISTS);
}

#else
/* ======================================================================== */
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/evp.h>

static int    s_argc;
static char **s_argv;

void gz_host_set_args(int argc, char **argv) { s_argc = argc; s_argv = argv; }

gzch **gz_host_args(int *argc)
{
    *argc = s_argc;
    return s_argv;
}

void gz_host_free_args(gzch **argv) { (void)argv; }

void gz_to_utf8(const gzch *p, char *out, int n) { snprintf(out, (size_t)n, "%s", p); }

int gz_from_text(const char *line, gzch *out, int n)
{
    snprintf(out, (size_t)n, "%s", line);
    return 1;
}

int gz_file_exists(const gzch *p) { struct stat st; return stat(p, &st) == 0 && !S_ISDIR(st.st_mode); }
int gz_dir_exists(const gzch *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
int gz_path_exists(const gzch *p) { struct stat st; return stat(p, &st) == 0; }

void gz_make_dirs(const gzch *path)
{
    char tmp[GZ_PATH_MAX], *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

int gz_delete_file(const gzch *p) { return unlink(p) == 0; }
int gz_move_replace(const gzch *a, const gzch *b) { return rename(a, b) == 0; }

int gz_copy_file_new(const gzch *a, const gzch *b)
{
    char buf[65536];
    ssize_t k;
    int in, out, ok = 1;
    if ((in = open(a, O_RDONLY)) < 0) return 0;
    if ((out = open(b, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) { close(in); return 0; }
    while ((k = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)k) != k) { ok = 0; break; }
    if (k < 0) ok = 0;
    close(in);
    if (close(out) != 0) ok = 0;
    return ok;
}

int gz_folder_writable(const gzch *dir) { return access(dir, W_OK) == 0; }

int gz_full_path(const gzch *in, gzch *out, int n)
{
    char cwd[GZ_PATH_MAX];
    char *r = realpath(in, NULL);
    if (r) { snprintf(out, (size_t)n, "%s", r); free(r); return 1; }
    if (in[0] == '/' || !getcwd(cwd, sizeof cwd)) { snprintf(out, (size_t)n, "%s", in); return 1; }
    snprintf(out, (size_t)n, "%s/%s", cwd, in);
    return 1;
}

int gz_chdir(const gzch *dir) { return chdir(dir) == 0; }

void gz_file_info(const gzch *p, unsigned long long *size, unsigned long long *mtime)
{
    struct stat st;
    if (stat(p, &st) != 0) { *size = 0; *mtime = 0; return; }
    *size = (unsigned long long)st.st_size;
    *mtime = (unsigned long long)st.st_mtime;
}

void gz_list_dir(const gzch *dir, gz_dir_fn fn, void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char p[GZ_PATH_MAX];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        fn(e->d_name, gz_dir_exists(p), ctx);
    }
    closedir(d);
}

void gz_exe_dir(gzch *out, int n)
{
    ssize_t k = readlink("/proc/self/exe", out, (size_t)n - 1);
    char *slash;
    if (k <= 0) { snprintf(out, (size_t)n, "."); return; }
    out[k] = 0;
    if ((slash = strrchr(out, '/')) != NULL) *slash = 0;
}

int gz_user_data_dir(gzch *out, int n)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, (size_t)n, "%s", x);
    else if (h && *h) snprintf(out, (size_t)n, "%s/.local/share", h);
    else return 0;
    return 1;
}

/* SDL2 is linked on POSIX (window, pads, audio); its message box needs no
 * window of ours. The text goes to the log too, for a run without a desktop. */
extern int SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window);

void gz_message_box(const gzch *text, const gzch *caption, int warning)
{
    fprintf(stderr, "[%s] %s\n", caption, text);
    SDL_ShowSimpleMessageBox(warning ? 0x20u /* SDL_MESSAGEBOX_WARNING */ : 0x40u /* _INFORMATION */,
                             caption, text, NULL);
}

void gz_sha1_hex(const void *data, size_t n, char out[41])
{
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned len = 0, i;
    out[0] = 0;
    if (!EVP_Digest(data, n, d, &len, EVP_sha1(), NULL) || len != 20) return;
    for (i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", d[i]);
}

void gz_list_dir_a(const char *dir, const char *pattern, gz_dir_fn_a fn, void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char p[GZ_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (fnmatch(pattern, e->d_name, FNM_CASEFOLD) == 0 && gz_file_exists(p)) fn(e->d_name, ctx);
    }
    closedir(d);
}

int gz_delete_file_a(const char *p) { return unlink(p) == 0; }
void gz_file_info_a(const char *p, unsigned long long *size, unsigned long long *mtime) { gz_file_info(p, size, mtime); }

int gz_single_instance(const char *key)
{
    char name[GZ_PATH_MAX], *c;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int fd;
    snprintf(name, sizeof name, "%s/gz_cache_%s.lock", (rt && *rt) ? rt : "/tmp", key);
    for (c = name + strlen((rt && *rt) ? rt : "/tmp") + 1; *c; c++) if (*c == '/' || *c == ':') *c = '_';
    if ((fd = open(name, O_RDWR | O_CREAT, 0600)) < 0) return 1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return 0; }
    return 1;                                   /* fd kept open for the whole run */
}
#endif
