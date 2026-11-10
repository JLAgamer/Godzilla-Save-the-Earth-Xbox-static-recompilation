/*
 * gz_profile.c - a tiny sampling profiler for the recomp.
 *
 * GZ_PROFILE=<out file>: once gz_profile_start() is called (the GPU back end
 * calls it a few seconds after a trace request, i.e. mid-fight), a thread
 * suspends every other thread in the process about 1000 times a second,
 * records its instruction pointer, and resumes it. After GZ_PROFILE_SECS
 * (default 8) it writes "<thread id> <module> <offset> <count>" lines to the
 * out file. tools: resolve offsets in the exe with build/Release/*.map.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSLOT 65536

typedef struct { DWORD tid; uintptr_t rip; uint32_t n; } Slot;

static Slot   *s_slots;
static volatile LONG s_started;

static void add(DWORD tid, uintptr_t rip)
{
    uint32_t h = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 40) ^ (tid * 2654435761u);
    uint32_t i;
    for (i = 0; i < NSLOT; i++) {
        Slot *s = &s_slots[(h + i) & (NSLOT - 1)];
        if (s->n && s->tid == tid && s->rip == rip) { s->n++; return; }
        if (!s->n) { s->tid = tid; s->rip = rip; s->n = 1; return; }
    }
}

static DWORD WINAPI sampler(LPVOID arg)
{
    const char *out = (const char *)arg;
    const char *secs_env = getenv("GZ_PROFILE_SECS");
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(secs_env ? atoi(secs_env) : 8) * 1000u;
    DWORD tids[256];
    HANDLE hs[256];
    int nt = 0, i;
    unsigned long samples = 0;
    FILE *f;

    timeBeginPeriod(1);
    /* The thread set is fixed by mid-fight; take it once. */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && nt < 256) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                          FALSE, te.th32ThreadID);
                    if (h) { tids[nt] = te.th32ThreadID; hs[nt] = h; nt++; }
                }
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    }
    fprintf(stderr, "[PROFILE] sampling %d threads\n", nt);
    while (GetTickCount64() < end) {
        for (i = 0; i < nt; i++) {
            CONTEXT c;
            if (SuspendThread(hs[i]) == (DWORD)-1) continue;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(hs[i], &c)) add(tids[i], (uintptr_t)c.Rip);
            ResumeThread(hs[i]);
        }
        samples++;
        Sleep(1);
    }
    timeEndPeriod(1);

    if ((f = fopen(out, "w"))) {
        uint32_t k;
        fprintf(f, "# rounds %lu threads %d\n", samples, nt);
        for (k = 0; k < NSLOT; k++) {
            Slot *s = &s_slots[k];
            char mod[MAX_PATH] = "?";
            HMODULE hm = NULL;
            uintptr_t base = 0;
            if (!s->n) continue;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)s->rip, &hm) && hm) {
                char *p;
                GetModuleFileNameA(hm, mod, sizeof mod);
                if ((p = strrchr(mod, '\\'))) memmove(mod, p + 1, strlen(p));
                base = (uintptr_t)hm;
            }
            fprintf(f, "%lu %s %llX %u\n", (unsigned long)s->tid, mod,
                    (unsigned long long)(s->rip - base), s->n);
        }
        fclose(f);
        fprintf(stderr, "[PROFILE] wrote %s (%lu rounds)\n", out, samples);
    }
    for (i = 0; i < nt; i++) CloseHandle(hs[i]);
    return 0;
}

/* Start the sampler once, if GZ_PROFILE is set. Safe to call repeatedly. */
void gz_profile_start(void)
{
    const char *out = getenv("GZ_PROFILE");
    HANDLE t;
    if (!out || InterlockedExchange(&s_started, 1)) return;
    s_slots = (Slot *)calloc(NSLOT, sizeof(Slot));
    if (!s_slots) return;
    t = CreateThread(NULL, 0, sampler, (LPVOID)out, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
}
