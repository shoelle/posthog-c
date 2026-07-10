/*
 * ph_time.h - clocks, ISO-8601 formatting, and UUIDv7 minting.
 *
 * Split deliberately from the capture path: ph_capture() reads only the cheap
 * monotonic counter (ph_now_mono_ns). Everything expensive or non-deterministic
 * - wall-clock correction, formatting, UUID generation - runs on the sender
 * thread. Each event snapshots the current wall/mono epoch at enqueue so later
 * clock corrections affect future events without shifting old queued records.
 */
#ifndef PH_TIME_H
#define PH_TIME_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic nanoseconds from an unspecified origin. Cheap, non-blocking, never
 * goes backwards. The one clock the capture hot path is allowed to read. */
uint64_t ph_now_mono_ns(void);

/* Wall-clock nanoseconds since the Unix epoch (UTC). Sender/init only. */
uint64_t ph_now_wall_ns(void);

/* A 64-bit seed from the operating system RNG for the per-init UUID salt and
 * reset IDs. Falls back to a clock/process mixer only when the OS source is
 * unavailable. Init/control path only. */
uint64_t ph_seed_u64(void);

/* Pure sender-side wall/monotonic calibration. If the wall time predicted by
 * the current epoch differs from now by more than threshold_ns, shift the wall
 * epoch by that skew. Capture still reads only the suspend-aware monotonic
 * clock. */
uint64_t ph_correct_wall_epoch(uint64_t epoch_wall_ns, uint64_t epoch_mono_ns,
                               uint64_t now_wall_ns, uint64_t now_mono_ns,
                               uint64_t threshold_ns);

/* Format wall-clock nanoseconds as "YYYY-MM-DDTHH:MM:SS.mmmZ" (millisecond
 * precision, always UTC). Writes at most `cap` bytes including the NUL. */
void ph_format_iso8601(uint64_t wall_ns, char *out, size_t cap);

/* Write a UUIDv7 (36 chars + NUL) into `out` (must hold >= 37 bytes). The 48-bit
 * timestamp is `wall_ms`; the random bits are derived deterministically from
 * (salt, seq) so the same event mints the same UUID across retries and offline
 * replay - the idempotency key PostHog dedups on. */
void ph_uuid_v7(uint64_t wall_ms, uint64_t salt, uint64_t seq, char out[37]);

#endif /* PH_TIME_H */
