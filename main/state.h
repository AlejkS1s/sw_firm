#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "timing.h"

/* ══════════════════════════════════════════════════════════════════════════
 * State Snapshot — packed struct of every cacheable state field.
 * Exposed here so the SSE module can cache a copy for delta encoding.
 * Must stay consistent with routines.h ROUTINES_MAX (12) and TZ_MAX_LEN.
 *
 * countdown_rem is deliberately the LAST field. It ticks once a second
 * while a countdown is running, but it's excluded from both the SSE wire
 * format and state_get_hash()'s ETag/dedup hash — the client derives the
 * remaining time from cd_target_epoch instead (see state_build_ssedata()
 * in state.c). Keeping it last lets state_get_hash() hash one contiguous
 * prefix of the struct instead of the whole thing. If you add a new
 * field, put it BEFORE countdown_rem unless it's similarly high-frequency
 * and client-derivable.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint8_t  relay;
    uint8_t  led;
    uint8_t  led_mode;
    uint8_t  boot_mode;
    uint8_t  time_ok;
    uint8_t  power_save_disabled;
    uint8_t  countdown_active;
    uint32_t countdown_total;
    uint32_t cd_target_epoch;
    uint8_t  routine_active_mask;
    uint8_t  auto_off_armed;
    uint8_t  auto_off_h;
    uint8_t  auto_off_m;
    uint8_t  auto_off_s;
    uint8_t  routine_count;
    uint8_t  routine_ids[12];   /* ROUTINES_MAX = 12 */
    char     tz[TZ_MAX_LEN];
    uint8_t  sse_enabled;       /* 1 = SSE active, 0 = disabled */
    uint32_t countdown_rem;     /* excluded from the hash — see note above */
} state_snapshot_t;

/* Build a fresh snapshot from the current device state. */
void state_snapshot_build(state_snapshot_t *s);

/* Returns the snapshot backing the current state_get_hash() value,
 * rebuilding it only if state has changed since the last call — the same
 * dirty-flag gate state_get_hash() uses internally. Calling both back to
 * back (the common SSE pattern: check the hash, then encode a delta
 * against the snapshot) costs one state_snapshot_build(), not two.
 * The returned pointer is only valid until the next state-mutating call
 * observed by this task; copy it out if you need to hold onto it (e.g.
 * as `prev` for the next delta). */
const state_snapshot_t *state_get_snapshot(void);

/* Build the SSE data line — a single base64-encoded binary token.
 * If prev is NULL, emits a full snapshot (f<base64>, 16 chars for 12 bytes).
 * If prev is non-NULL, emits a delta (d<base64>).
 * Countdown remaining time is derived client-side from cd_target_epoch.
 * Returns bytes written including NUL, 0 on truncation or no change. */
size_t state_build_ssedata(char *buf, size_t buflen,
                            const state_snapshot_t *prev,
                            const state_snapshot_t *cur);

void state_init(void);
void  notify_bump_state(void);

/* External modules can register a callback invoked inside every
 * notify_bump_state() call. Used by the SSE module to push state
 * updates to connected browsers without creating a circular include
 * dependency. */
typedef void (*state_change_cb_t)(void);
void state_register_on_change(state_change_cb_t cb);

/* Block until the state version changes, up to `ticks` timeout.
 * Returns true if the state has changed since the last known hash,
 * false on timeout.  Used by the SSE handler instead of touching the
 * event group directly. */
bool state_wait_for_change(uint32_t last_hash, TickType_t ticks);

volatile uint32_t g_state_version;

/* Wraps the snprintf return-value guard that appears in every JSON builder.
 * Returns the written byte count on success, 0 on truncation/error. */
static inline size_t snprintf_guard(int n, size_t buflen) {
    return (n < 0 || (size_t)n >= buflen) ? 0 : (size_t)n;
}

/* ══════════════════════════════════════════════════════════════════════════
 * State hashing & serialization — the caching contract between state.c
 * and anything that exposes state over the network (http_server.c today).
 *
 * Two deliberately separate things live here:
 *
 *   state_get_hash()   — a content fingerprint over the CACHEABLE subset
 *                         of state (relay/led/boot/timer/routines/tz/...).
 *                         Volatile telemetry (uptime, heap, rssi) is
 *                         excluded on purpose: those change every request,
 *                         and hashing them would make the cache useless
 *                         (every poll would look "changed"). countdown_rem
 *                         is excluded for the same reason — see the note
 *                         on state_snapshot_t in this header.
 *
 *   state_build_json() — the wire payload for GET /api/v1/state. This DOES
 *                         include uptime, because the UI displays it — it's
 *                         just not part of the hash. SSE pushes use the
 *                         separate, more compact state_build_ssedata()
 *                         binary encoder below, not this function.
 *
 * Do not conflate the two: never hash the output of state_build_json()
 * directly.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Lazily recomputed: cheap on the common path (nothing changed since the
 * last call → just returns the cached value). Recomputation only happens
 * once per notify_bump_state() call, no matter how many times this is
 * polled in between. */
uint32_t state_get_hash(void);

/* Serializes the current cacheable state (the same fields covered by
 * state_get_hash, plus display-only telemetry like uptime) as a JSON
 * object into buf. Returns the number of bytes written (excluding the
 * NUL terminator), or 0 if the buffer was too small. */
size_t state_build_json(char *buf, size_t buflen);