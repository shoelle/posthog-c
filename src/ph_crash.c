/*
 * ph_crash.c - in-process signal_crash handler (see ph_crash.h).
 *
 * Two worlds live here, kept strictly apart:
 *   - the handler (posix_handler / win_filter): runs in a dying process, so it
 *     stays lean with no explicit SDK allocation - it snapshots the stack as (module,
 *     offset) pairs into a static record and write()s it. The one concession is
 *     a loader query per frame (dladdr / GetModuleHandleEx) to find each
 *     module's base. backtrace/dladdr are not async-signal-safe and may allocate
 *     or take the loader lock, so heap/loader crashes can stall. That's the
 *     known limit of in-process capture and exactly why
 *     robust capture is out-of-process (minidump_crash); acceptable here.
 *   - everything else (replay): runs in normal context on the next launch and
 *     reuses the posthog_exception path (ph_capture_exception) rather than
 *     re-implementing $exception building.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "ph_crash.h"
#include "posthog.h"
#include "ph_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h> /* SIG* names for ph_crash_signal_name on all platforms */

#if defined(_WIN32)
#include <direct.h>
static void crash_ensure_dir(const char *dir) {
    if (dir && dir[0]) _mkdir(dir);
}
#else
#include <sys/stat.h>
#include <sys/types.h>
static void crash_ensure_dir(const char *dir) {
    if (dir && dir[0]) mkdir(dir, 0700);
}
#endif

/* --- per-frame module lookup (base + basename for an address) ---------- */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int addr_module(uint64_t addr, char *name, size_t cap, uint64_t *base) {
    HMODULE h = NULL;
    char path[MAX_PATH];
    const char *b, *p;
    size_t k;
    *base = 0;
    name[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)addr, &h) ||
        !h)
        return 0;
    *base = (uint64_t)(uintptr_t)h;
    if (GetModuleFileNameA(h, path, sizeof path) == 0) return 1; /* base, no name */
    for (b = p = path; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    for (k = 0; b[k] && k + 1 < cap; k++) name[k] = b[k];
    name[k] = 0;
    return 1;
}

#elif defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h> /* dladdr (needs _GNU_SOURCE on glibc; set via -D in build) */

static int addr_module(uint64_t addr, char *name, size_t cap, uint64_t *base) {
    Dl_info info;
    const char *b, *p;
    size_t k;
    *base = 0;
    name[0] = 0;
    if (!dladdr((void *)(uintptr_t)addr, &info) || !info.dli_fbase) return 0;
    *base = (uint64_t)(uintptr_t)info.dli_fbase;
    if (info.dli_fname && info.dli_fname[0]) {
        for (b = p = info.dli_fname; *p; p++)
            if (*p == '/' || *p == '\\') b = p + 1;
        for (k = 0; b[k] && k + 1 < cap; k++) name[k] = b[k];
        name[k] = 0;
    }
    return 1;
}

#else
static int addr_module(uint64_t addr, char *name, size_t cap, uint64_t *base) {
    (void)addr;
    (void)cap;
    *base = 0;
    name[0] = 0;
    return 0;
}
#endif

/* --- record codec (little-endian, no struct padding assumptions) ------- */

static const char CRASH_MAGIC[4] = { 'P', 'H', 'C', 'R' };
#define CRASH_VERSION 2

static void crash_path(const char *dir, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", dir, PH_CRASH_FILENAME);
}

static void discard_crash_record(const char *dir, const char *reason) {
    char path[PH_PATH_CAP + 32];
    if (!dir || !dir[0]) return;
    crash_path(dir, path, sizeof path);
    if (remove(path) == 0)
        ph_log(PH_LOG_WARN, "signal_crash: discarded replay record (%s)", reason);
}

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void put_u32(unsigned char *p, uint32_t v) {
    int i;
    for (i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void put_u64(unsigned char *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static size_t bounded_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

size_t ph_crash_encode(const ph_crash_info *in, char *buf, size_t cap) {
    unsigned char *p = (unsigned char *)buf;
    size_t o, need;
    int i, mc = in->module_count, fc = in->frame_count;
    size_t mlen[PH_CRASH_MAX_MODULES];
    if (mc < 0) mc = 0;
    if (mc > PH_CRASH_MAX_MODULES) mc = PH_CRASH_MAX_MODULES;
    if (fc < 0) fc = 0;
    if (fc > PH_CRASH_MAX_FRAMES) fc = PH_CRASH_MAX_FRAMES;

    need = 20; /* magic+ver+sig+fault_addr+module_count */
    for (i = 0; i < mc; i++) {
        mlen[i] = bounded_len(in->modules[i], PH_CRASH_MODULE_NAME_CAP);
        need += 2 + mlen[i];
    }
    need += 2 + (size_t)fc * (2 + 8); /* frame_count + frames */
    if (cap < need) return 0;

    memcpy(p, CRASH_MAGIC, 4);
    put_u16(p + 4, CRASH_VERSION);
    put_u32(p + 6, (uint32_t)in->sig);
    put_u64(p + 10, in->fault_addr);
    put_u16(p + 18, (uint16_t)mc);
    o = 20;
    for (i = 0; i < mc; i++) {
        put_u16(p + o, (uint16_t)mlen[i]);
        o += 2;
        memcpy(p + o, in->modules[i], mlen[i]);
        o += mlen[i];
    }
    put_u16(p + o, (uint16_t)fc);
    o += 2;
    for (i = 0; i < fc; i++) {
        int mi = in->frame_module[i];
        put_u16(p + o, (uint16_t)(mi < 0 ? 0xFFFF : mi));
        o += 2;
        put_u64(p + o, in->frame_off[i]);
        o += 8;
    }
    return o;
}

int ph_crash_decode(const char *buf, size_t len, ph_crash_info *out) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t o;
    uint16_t mc, fc, i;
    if (len < 20 || memcmp(p, CRASH_MAGIC, 4) != 0) return 0;
    if (get_u16(p + 4) != CRASH_VERSION) return 0;
    out->sig = (int)get_u32(p + 6);
    out->fault_addr = get_u64(p + 10);
    mc = get_u16(p + 18);
    if (mc > PH_CRASH_MAX_MODULES) return 0;
    o = 20;
    for (i = 0; i < mc; i++) {
        uint16_t nl;
        if (o + 2 > len) return 0;
        nl = get_u16(p + o);
        o += 2;
        if (nl >= PH_CRASH_MODULE_NAME_CAP || o + nl > len) return 0;
        memcpy(out->modules[i], p + o, nl);
        out->modules[i][nl] = 0;
        o += nl;
    }
    out->module_count = (int)mc;
    if (o + 2 > len) return 0;
    fc = get_u16(p + o);
    o += 2;
    if (fc > PH_CRASH_MAX_FRAMES) return 0;
    if (o + (size_t)fc * (2 + 8) > len) return 0;
    for (i = 0; i < fc; i++) {
        uint16_t mi = get_u16(p + o);
        o += 2;
        out->frame_module[i] = (mi == 0xFFFF || mi >= mc) ? -1 : (int)mi;
        out->frame_off[i] = get_u64(p + o);
        o += 8;
    }
    out->frame_count = (int)fc;
    return 1;
}

const char *ph_crash_signal_name(int sig) {
    switch ((unsigned)sig) { /* Windows SEH exception codes */
    case 0xC0000005u: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC000001Du: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000094u: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC00000FDu: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000096u: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC000008Cu: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC0000374u: return "EXCEPTION_HEAP_CORRUPTION";
    case 0x80000003u: return "EXCEPTION_BREAKPOINT";
    default: break;
    }
    switch (sig) { /* POSIX signals */
#ifdef SIGSEGV
    case SIGSEGV: return "SIGSEGV";
#endif
#ifdef SIGABRT
    case SIGABRT: return "SIGABRT";
#endif
#ifdef SIGBUS
    case SIGBUS: return "SIGBUS";
#endif
#ifdef SIGFPE
    case SIGFPE: return "SIGFPE";
#endif
#ifdef SIGILL
    case SIGILL: return "SIGILL";
#endif
#ifdef SIGTRAP
    case SIGTRAP: return "SIGTRAP";
#endif
    default: return "SIGNAL";
    }
}

/* Absolute stack addresses -> (module, offset) frames, deduping module names
 * into the table. Runs in the crash handler, so it only reads the loader map. */
static void fill_frames(ph_crash_info *ci, const uint64_t *pcs, int n) {
    int i;
    ci->module_count = 0;
    ci->frame_count = 0;
    for (i = 0; i < n && ci->frame_count < PH_CRASH_MAX_FRAMES; i++) {
        char name[PH_CRASH_MODULE_NAME_CAP];
        uint64_t base = 0;
        int idx = -1;
        if (addr_module(pcs[i], name, sizeof name, &base) && name[0]) {
            int m;
            for (m = 0; m < ci->module_count; m++)
                if (strcmp(ci->modules[m], name) == 0) {
                    idx = m;
                    break;
                }
            if (idx < 0 && ci->module_count < PH_CRASH_MAX_MODULES) {
                size_t k;
                idx = ci->module_count++;
                for (k = 0; name[k] && k + 1 < PH_CRASH_MODULE_NAME_CAP; k++)
                    ci->modules[idx][k] = name[k];
                ci->modules[idx][k] = 0;
            }
        }
        ci->frame_module[ci->frame_count] = idx;
        ci->frame_off[ci->frame_count] = (idx >= 0) ? (pcs[i] - base) : pcs[i];
        ci->frame_count++;
    }
}

/* --- replay (normal context, next launch) ------------------------------ */

int ph_signal_crash_replay(const char *dir) {
    char path[PH_PATH_CAP + 32];
    char buf[PH_CRASH_RECORD_MAX];
    FILE *f;
    size_t rd;
    ph_crash_info ci;
    const char *name;
    char message[PH_EXCEPTION_FIELD_CAP];
    ph_exception ex;
    ph_props extra;
    ph_stackframe frames[PH_MAX_EXCEPTION_FRAMES];
    char fbuf[PH_MAX_EXCEPTION_FRAMES][24]; /* "0x" + 16 hex + NUL */
    int nf, i;

    if (!dir || !dir[0]) return 0;
    crash_path(dir, path, sizeof path);
    f = fopen(path, "rb");
    if (!f) return 0;
    rd = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (!ph_crash_decode(buf, rd, &ci)) {
        discard_crash_record(dir, "corrupt record");
        return 0;
    }

    name = ph_crash_signal_name(ci.sig);
    snprintf(message, sizeof message, "%s at 0x%016llx", name,
             (unsigned long long)ci.fault_addr);

    nf = ci.frame_count;
    if (nf > PH_MAX_EXCEPTION_FRAMES) nf = PH_MAX_EXCEPTION_FRAMES;
    for (i = 0; i < nf; i++) {
        int mi = ci.frame_module[i];
        /* function carries the module-relative offset (a symbol server turns it
         * into a name); module carries the basename. */
        snprintf(fbuf[i], sizeof fbuf[i], "0x%llx",
                 (unsigned long long)ci.frame_off[i]);
        frames[i].function = fbuf[i];
        frames[i].filename = NULL;
        frames[i].module = (mi >= 0) ? ci.modules[mi] : NULL;
        frames[i].lineno = 0;
        frames[i].in_app = 1;
    }

    /* Tag the origin so PostHog can tell an app-reported posthog_exception from
     * a signal_crash. handled=0: this process did not survive. */
    ph_props_init(&extra);
    ph_props_set_str(&extra, "crash_origin", "signal_crash");

    memset(&ex, 0, sizeof ex);
    ex.type = name;
    ex.message = message;
    ex.handled = 0;
    ex.synthetic = 0;
    ex.frames = frames;
    ex.frame_count = nf;
    ex.extra = &extra;
    if (ph__capture_exception_flags(&ex, PH_EVF_CRASH_REPLAY) != PH_OK) {
        /* The exception was refused at capture - a before_send that drops
         * $exception, or a record that decoded to an invalid exception. That is
         * deterministic: retaining the record would re-offer and re-refuse it on
         * every launch forever. Discard it as terminally handled. */
        discard_crash_record(dir, "capture veto");
        return 0;
    }

    ph_log(PH_LOG_WARN, "signal_crash: queued a %s from a previous run", name);
    return 1;
}

void ph_signal_crash_handoff_complete(const char *dir) {
    char path[PH_PATH_CAP + 32];
    if (!dir || !dir[0]) return;
    crash_path(dir, path, sizeof path);
    if (remove(path) == 0)
        ph_log(PH_LOG_INFO, "signal_crash: durable replay handoff complete");
}

/* --- OS handler: POSIX ------------------------------------------------- */

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <ucontext.h>

static char g_posix_path[PH_PATH_CAP + 32];
static char g_posix_tmp_path[PH_PATH_CAP + 40];
static int g_posix_installed;
static stack_t g_altstack;
static stack_t g_prev_altstack;
static int g_prev_altstack_valid;
static int g_altstack_owned;
static volatile sig_atomic_t g_in_handler;
static const int g_posix_sigs[] = {
#ifdef SIGSEGV
    SIGSEGV,
#endif
#ifdef SIGABRT
    SIGABRT,
#endif
#ifdef SIGBUS
    SIGBUS,
#endif
#ifdef SIGFPE
    SIGFPE,
#endif
#ifdef SIGILL
    SIGILL,
#endif
    0
};
static struct sigaction g_posix_prev[sizeof(g_posix_sigs) / sizeof(g_posix_sigs[0])];
static unsigned char g_posix_prev_valid[sizeof(g_posix_sigs) / sizeof(g_posix_sigs[0])];

/* Faulting instruction pointer from the crash context, so it leads the trace
 * (backtrace()'s own top frames are this handler + the signal trampoline).
 * Arch-specific; 0 where we don't special-case, which falls back to backtrace. */
static uint64_t posix_fault_pc(void *uctx) {
#if defined(__linux__) && defined(__x86_64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (uint64_t)((ucontext_t *)uctx)->uc_mcontext.pc;
#else
    /* macOS reaches the fault PC through uc_mcontext, a pointer to a separately
     * allocated mcontext. Reading __ss.__pc/__rip through it faulted inside the
     * handler on the arm64 CI runner - re-entering the handler (g_in_handler)
     * and aborting capture with _exit(134) before the record was written. Fall
     * back to backtrace() on macOS: its top frames are this handler + the signal
     * trampoline, so the faulting instruction simply isn't hoisted to the front
     * of the trace. TODO: restore an alignment-safe macOS PC read once it can be
     * validated on real hardware. */
    (void)uctx;
    return 0;
#endif
}

static void posix_restore_for_signal(int sig) {
    int idx;
    for (idx = 0; g_posix_sigs[idx]; idx++) {
        if (g_posix_sigs[idx] == sig && g_posix_prev_valid[idx]) {
            sigaction(sig, &g_posix_prev[idx], NULL);
            return;
        }
    }
    signal(sig, SIG_DFL);
}

static void posix_handler(int sig, siginfo_t *si, void *uctx) {
    static ph_crash_info ci; /* static: the handler burns almost no stack */
    static char rec[PH_CRASH_RECORD_MAX];
    void *addrs[PH_CRASH_MAX_FRAMES];
    uint64_t pcs[PH_CRASH_MAX_FRAMES];
    int n, fd, i, np = 0;
    size_t len;
    uint64_t pc;
    if (g_in_handler) _exit(134); /* a fault inside the handler: bail hard */
    g_in_handler = 1;

    n = backtrace(addrs, PH_CRASH_MAX_FRAMES);
    pc = posix_fault_pc(uctx);
    if (pc) pcs[np++] = pc;
    for (i = 0; i < n && np < PH_CRASH_MAX_FRAMES; i++)
        pcs[np++] = (uint64_t)(uintptr_t)addrs[i];

    ci.sig = sig;
    ci.fault_addr = si ? (uint64_t)(uintptr_t)si->si_addr : 0;
    fill_frames(&ci, pcs, np);

    len = ph_crash_encode(&ci, rec, sizeof rec);
    if (len) {
        fd = open(g_posix_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            size_t off = 0;
            while (off < len) {
                ssize_t w = write(fd, rec + off, len - off);
                if (w > 0) off += (size_t)w;
                else break;
            }
            if (off == len && fsync(fd) == 0) {
                close(fd);
                fd = -1;
                /* Hard-link publication is atomic and refuses to replace an
                 * older unacknowledged crash record. A second crash must not
                 * erase the first report before its durable handoff. */
                (void)link(g_posix_tmp_path, g_posix_path);
                (void)unlink(g_posix_tmp_path);
            }
            if (fd >= 0) close(fd);
        }
    }
    /* Re-raise into the host's previous disposition so an existing crash handler
     * (or the default core-dump path) still runs after our record is persisted.
     * The triggering signal is masked until this handler returns, so raise()
     * queues it for delivery under the restored disposition. */
    posix_restore_for_signal(sig);
    raise(sig);
}

int ph_signal_crash_install(const char *dir) {
    struct sigaction sa;
    int idx;
    void *warm[4];
    if (!dir || !dir[0]) return 0;
    if (g_posix_installed) return 1;
    crash_ensure_dir(dir);
    crash_path(dir, g_posix_path, sizeof g_posix_path);
    snprintf(g_posix_tmp_path, sizeof g_posix_tmp_path, "%s.tmp", g_posix_path);

    /* Warm backtrace() so its lazy first call (which may dlopen/malloc) happens
     * here, not inside the signal handler where malloc is forbidden. */
    (void)backtrace(warm, 4);

    /* A generous alternate stack lets a stack-overflow SIGSEGV still run the
     * handler (bigger than SIGSTKSZ so the loader queries have room). */
    g_prev_altstack_valid = sigaltstack(NULL, &g_prev_altstack) == 0;
    g_altstack_owned = 0;
    g_altstack.ss_size = 64 * 1024;
    g_altstack.ss_sp = malloc(g_altstack.ss_size);
    if (g_altstack.ss_sp) {
        g_altstack.ss_flags = 0;
        if (sigaltstack(&g_altstack, NULL) == 0)
            g_altstack_owned = 1;
        else {
            free(g_altstack.ss_sp);
            g_altstack.ss_sp = NULL;
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = posix_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    memset(g_posix_prev_valid, 0, sizeof g_posix_prev_valid);
    for (idx = 0; g_posix_sigs[idx]; idx++)
        if (sigaction(g_posix_sigs[idx], &sa, &g_posix_prev[idx]) == 0)
            g_posix_prev_valid[idx] = 1;
    g_posix_installed = 1;
    return 1;
}

void ph_signal_crash_uninstall(void) {
    int idx;
    if (!g_posix_installed) return;
    for (idx = 0; g_posix_sigs[idx]; idx++) {
        struct sigaction current;
        if (g_posix_prev_valid[idx] &&
            sigaction(g_posix_sigs[idx], NULL, &current) == 0 &&
            (current.sa_flags & SA_SIGINFO) &&
            current.sa_sigaction == posix_handler)
            sigaction(g_posix_sigs[idx], &g_posix_prev[idx], NULL);
    }
    if (g_altstack_owned && g_altstack.ss_sp) {
        stack_t current;
        /* Do not replace an alternate stack a host installed after us. */
        if (sigaltstack(NULL, &current) == 0 &&
            current.ss_sp == g_altstack.ss_sp) {
            if (g_prev_altstack_valid)
                sigaltstack(&g_prev_altstack, NULL);
            else {
                stack_t off;
                memset(&off, 0, sizeof off);
                off.ss_flags = SS_DISABLE;
                sigaltstack(&off, NULL);
            }
        }
        free(g_altstack.ss_sp);
        g_altstack.ss_sp = NULL;
    }
    g_altstack_owned = 0;
    g_prev_altstack_valid = 0;
    g_posix_installed = 0;
}

/* --- OS handler: Windows SEH ------------------------------------------- */

#elif defined(_WIN32)

static char g_win_path[PH_PATH_CAP + 32];
static char g_win_tmp_path[PH_PATH_CAP + 40];
static int g_win_installed;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static volatile LONG g_in_filter;

static LONG WINAPI win_filter(EXCEPTION_POINTERS *ep) {
    static ph_crash_info ci;
    static char rec[PH_CRASH_RECORD_MAX];
    void *addrs[PH_CRASH_MAX_FRAMES];
    uint64_t pcs[PH_CRASH_MAX_FRAMES];
    USHORT n;
    int i, np = 0;
    size_t len;
    HANDLE h;
    if (InterlockedExchange(&g_in_filter, 1) != 0)
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;

    n = CaptureStackBackTrace(0, PH_CRASH_MAX_FRAMES, addrs, NULL);
    /* Faulting instruction first (CaptureStackBackTrace's top frames are this
     * filter + the exception dispatcher), so grouping keys on the real site. */
#if defined(_M_X64) || defined(__x86_64__)
    if (ep->ContextRecord) pcs[np++] = (uint64_t)ep->ContextRecord->Rip;
#elif defined(_M_ARM64) || defined(__aarch64__)
    if (ep->ContextRecord) pcs[np++] = (uint64_t)ep->ContextRecord->Pc;
#endif
    for (i = 0; i < n && np < PH_CRASH_MAX_FRAMES; i++)
        pcs[np++] = (uint64_t)(uintptr_t)addrs[i];

    ci.sig = (int)ep->ExceptionRecord->ExceptionCode;
    ci.fault_addr = ep->ExceptionRecord->NumberParameters >= 2
                        ? (uint64_t)ep->ExceptionRecord->ExceptionInformation[1]
                        : 0;
    fill_frames(&ci, pcs, np);

    len = ph_crash_encode(&ci, rec, sizeof rec);
    if (len) {
        h = CreateFileA(g_win_tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            BOOL ok = WriteFile(h, rec, (DWORD)len, &wrote, NULL) &&
                      wrote == (DWORD)len && FlushFileBuffers(h);
            CloseHandle(h);
            if (ok)
                /* No REPLACE_EXISTING: preserve an older unacknowledged crash. */
                MoveFileExA(g_win_tmp_path, g_win_path, MOVEFILE_WRITE_THROUGH);
        }
    }
    /* Chain to the previous filter (e.g. WER) so normal crash handling runs. */
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

int ph_signal_crash_install(const char *dir) {
    if (!dir || !dir[0]) return 0;
    if (g_win_installed) return 1;
    crash_ensure_dir(dir);
    crash_path(dir, g_win_path, sizeof g_win_path);
    snprintf(g_win_tmp_path, sizeof g_win_tmp_path, "%s.tmp", g_win_path);
    g_prev_filter = SetUnhandledExceptionFilter(win_filter);
    g_win_installed = 1;
    return 1;
}

void ph_signal_crash_uninstall(void) {
    LPTOP_LEVEL_EXCEPTION_FILTER current;
    if (!g_win_installed) return;
    current = SetUnhandledExceptionFilter(g_prev_filter);
    /* A host may install a newer filter after posthog-c. Put it back instead
     * of silently clobbering it during SDK shutdown. */
    if (current != win_filter) SetUnhandledExceptionFilter(current);
    g_prev_filter = NULL;
    g_in_filter = 0;
    g_win_installed = 0;
}

#else /* unsupported platform: no-op hooks (replay still works) */

int ph_signal_crash_install(const char *dir) {
    (void)dir;
    return 0;
}
void ph_signal_crash_uninstall(void) {}

#endif
