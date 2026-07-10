/*
 * ph_crash.h - in-process signal_crash handler (v0.6), native only.
 *
 * Turns a fatal native fault (a POSIX signal or a Windows SEH exception) into a
 * persisted PostHog $exception that ships on the *next* launch. Naming (see the
 * posthog-exception-naming convention): the three origins of a $exception are
 *   - posthog_exception : app called ph_capture_exception (handled/reported)
 *   - signal_crash      : this file's in-process handler (v0.6)
 *   - minidump_crash    : out-of-process Crashpad/minidump (future; the server
 *                         side is the separate posthog-crash service)
 *
 * The handler runs inside a corrupted, dying process, so it does the minimum:
 * snapshot the signal, the faulting address, and the stack as (module, offset)
 * pairs into a fixed record, then write() it. Capturing module + offset (not a
 * raw absolute address) is what makes the frames meaningful across the restart:
 * ASLR relocates modules between runs, so an absolute address from the crashed
 * process is nonsense in the next one - but "myapp.exe + 0x1361" is stable, and
 * a symbol server can resolve it. Stack walking and loader lookup are not
 * async-signal-safe, however: this in-process mode is explicitly best-effort
 * and may fail during heap/loader corruption. Turning offsets into function
 * names is the minidump_crash server's job; the SDK stops at capture.
 *
 * All of the above happens later, in normal context, on the next run:
 * ph_signal_crash_replay() decodes the record into a ph_exception and hands it
 * to ph_capture_exception(), reusing the whole posthog_exception path.
 */
#ifndef PH_CRASH_H
#define PH_CRASH_H

#include <stddef.h>
#include <stdint.h>

/* Origin of a $exception event. All three serialize to the same wire shape;
 * this only tags where the exception came from. */
typedef enum {
    PH_ORIGIN_POSTHOG_EXCEPTION = 0, /* ph_capture_exception (app-reported, handled) */
    PH_ORIGIN_SIGNAL_CRASH,          /* in-process signal / SEH handler (this file) */
    PH_ORIGIN_MINIDUMP_CRASH         /* out-of-process minidump (future; posthog-crash) */
} ph_origin;

/* Stack addresses captured per crash; replay emits at most PH_MAX_EXCEPTION_FRAMES. */
#ifndef PH_CRASH_MAX_FRAMES
#define PH_CRASH_MAX_FRAMES 64
#endif
/* Distinct modules a crash stack can span (exe + a few libs is typical). */
#ifndef PH_CRASH_MAX_MODULES
#define PH_CRASH_MAX_MODULES 16
#endif
/* Module basename cap (bounded like every other per-event string). */
#ifndef PH_CRASH_MODULE_NAME_CAP
#define PH_CRASH_MODULE_NAME_CAP 64
#endif

/* --- OS hook (implemented per-platform; no-ops where unsupported) ------ */

/* Arm the in-process signal_crash handler; a crash record is written under
 * `dir` (the offline directory). Idempotent. Returns 1 if armed, else 0. */
int ph_signal_crash_install(const char *dir);

/* Restore the previous fault disposition. Idempotent; safe if never armed. */
void ph_signal_crash_uninstall(void);

/* If a signal_crash record from a previous run is present under `dir`, decode it
 * into a marked $exception. The sender removes the record only after delivery
 * or durable offline spill. Normal context. Returns the number enqueued (0/1). */
int ph_signal_crash_replay(const char *dir);

/* Sender acknowledgement after a marked replay event is durably handed off. */
void ph_signal_crash_handoff_complete(const char *dir);

/* --- pure helpers, exposed for unit tests ----------------------------- */

/* A decoded crash: the raw material replay turns into a ph_exception. Each
 * frame is a module index (into modules[], or -1 if the address matched no
 * loaded module) plus an offset within that module (or the absolute address
 * when the module is unknown). */
typedef struct ph_crash_info {
    int      sig;        /* POSIX signal number, or SEH exception code */
    uint64_t fault_addr; /* faulting address, 0 if n/a */
    int      module_count;
    char     modules[PH_CRASH_MAX_MODULES][PH_CRASH_MODULE_NAME_CAP];
    int      frame_count;
    int      frame_module[PH_CRASH_MAX_FRAMES]; /* index into modules[], or -1 */
    uint64_t frame_off[PH_CRASH_MAX_FRAMES];    /* offset in module (or absolute) */
} ph_crash_info;

/* Encode/decode the crash record - the wire between the signal handler and the
 * next run's replay. encode returns bytes written (0 on overflow); decode
 * returns 1 on a valid record, 0 otherwise. Pure; unit-tested directly. */
size_t ph_crash_encode(const ph_crash_info *in, char *buf, size_t cap);
int    ph_crash_decode(const char *buf, size_t len, ph_crash_info *out);

/* Bytes ph_crash_encode needs for a full record (upper bound). */
#define PH_CRASH_RECORD_MAX                                        \
    (20 + PH_CRASH_MAX_MODULES * (2 + PH_CRASH_MODULE_NAME_CAP) + 2 + \
     PH_CRASH_MAX_FRAMES * (2 + 8))

/* Basename of the crash record inside the offline dir (exposed for tests). */
#define PH_CRASH_FILENAME "ph-crash.bin"

/* Human-readable fault name ("SIGSEGV", "EXCEPTION_ACCESS_VIOLATION", ...).
 * Always returns a non-NULL static string. */
const char *ph_crash_signal_name(int sig);

#endif /* PH_CRASH_H */
