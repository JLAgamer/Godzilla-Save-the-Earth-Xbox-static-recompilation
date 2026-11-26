/*
 * gz_host.h - the few host services the game-side files need, on Windows and
 * on POSIX (Linux / Steam Deck).
 *
 * Paths are kept in the host's native form: UTF-16 (wchar_t) on Windows, so
 * any folder name works as before, and UTF-8 char on POSIX. gzch is that
 * character type, GZT("x") a literal of it and GZ_SEP the folder separator.
 * On Windows every macro and wrapper here expands to exactly the call the
 * code made before this header existed.
 *
 * Narrow (always char) paths, as main.c and the kernel's path layer use, take
 * GZ_SEP_A.
 */
#ifndef GZ_HOST_H
#define GZ_HOST_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <wchar.h>
typedef wchar_t gzch;
#define GZT_(s)       L##s
#define GZT(s)        GZT_(s)
#define GZ_SEP        L"\\"
#define GZ_SEPC       L'\\'
#define GZ_SEP_A      "\\"
#define GZ_PATH_MAX   260
#define gz_fopen      _wfopen
#define gz_freopen    _wfreopen
#define gz_sprintf    swprintf_s
#define gz_strlen     wcslen
#define gz_strcmp     wcscmp
#define gz_stricmp    _wcsicmp
#define gz_strrchr    wcsrchr
#define gz_strcpy(d, n, s)  wcsncpy_s((d), (n), (s), _TRUNCATE)
#define gz_strcat(d, n, s)  wcscat_s((d), (n), (s))
#define gz_fseek      _fseeki64
#define gz_ftell      _ftelli64
#else
#include <strings.h>
#include <stdlib.h>
typedef char gzch;
#define GZT(s)        s
#define GZ_SEP        "/"
#define GZ_SEPC       '/'
#define GZ_SEP_A      "/"
#define GZ_PATH_MAX   4096
#define gz_fopen      fopen
#define gz_freopen    freopen
#define gz_sprintf    snprintf
#define gz_strlen     strlen
#define gz_strcmp     strcmp
#define gz_stricmp    strcasecmp
#define gz_strrchr    strrchr
#define gz_strcpy(d, n, s)  ((void)snprintf((d), (n), "%s", (s)))
#define gz_strcat(d, n, s)  ((void)strncat((d), (s), (n) - strlen(d) - 1))
#define gz_fseek      fseeko
#define gz_ftell      ftello

/* The CRT spellings the game-side files use, for files that do not pull in
 * the toolkit's Win32 layer. */
#ifndef _stricmp
#define _stricmp      strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp     strncasecmp
#endif
#define _putenv_s(name, value)  setenv((name), (value), 1)
#endif

/* Command line as the host passed it (Windows: CommandLineToArgvW). POSIX
 * needs main() to hand it over first with gz_host_set_args. */
void   gz_host_set_args(int argc, char **argv);
gzch **gz_host_args(int *argc);
void   gz_host_free_args(gzch **argv);

void gz_to_utf8(const gzch *path, char *out, int n);
/* A line from a text file the player wrote (game_path.txt): OEM code page on
 * Windows, as Play.bat writes it; UTF-8 on POSIX. 0 on failure. */
int  gz_from_text(const char *line, gzch *out, int n);

int  gz_file_exists(const gzch *p);            /* a file, not a folder */
int  gz_dir_exists(const gzch *p);
int  gz_path_exists(const gzch *p);            /* either */
void gz_make_dirs(const gzch *path);           /* every folder in path */
int  gz_delete_file(const gzch *p);
int  gz_move_replace(const gzch *from, const gzch *to);
int  gz_copy_file_new(const gzch *from, const gzch *to);   /* fails if `to` exists */
int  gz_folder_writable(const gzch *dir);
int  gz_full_path(const gzch *in, gzch *out, int n);
int  gz_chdir(const gzch *dir);
/* size and last-write time; 0 for both when the file can't be read */
void gz_file_info(const gzch *p, unsigned long long *size, unsigned long long *mtime);

/* Calls fn for every entry of dir except . and .. */
typedef void (*gz_dir_fn)(const gzch *name, int is_dir, void *ctx);
void gz_list_dir(const gzch *dir, gz_dir_fn fn, void *ctx);

/* The folder the program is in. */
void gz_exe_dir(gzch *out, int n);
/* The per-user data folder saves go to by default: %LOCALAPPDATA% on
 * Windows, $XDG_DATA_HOME (else ~/.local/share) on POSIX. 0 if unknown. */
int  gz_user_data_dir(gzch *out, int n);

void gz_message_box(const gzch *text, const gzch *caption, int warning);
void gz_sha1_hex(const void *data, size_t n, char out[41]);

/* Files in a narrow-path folder matching a wildcard pattern (main.c's cache;
 * case-insensitive on POSIX too, like the Windows match). */
typedef void (*gz_dir_fn_a)(const char *name, void *ctx);
void gz_list_dir_a(const char *dir, const char *pattern, gz_dir_fn_a fn, void *ctx);
int  gz_delete_file_a(const char *p);
/* last-write time and size of a narrow path, 0 when missing */
void gz_file_info_a(const char *p, unsigned long long *size, unsigned long long *mtime);
/* Holds a lock named after `key` for the rest of the run. 0 when another
 * running copy holds it already. */
int  gz_single_instance(const char *key);

#endif
