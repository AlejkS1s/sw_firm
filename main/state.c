#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_timer.h"

#include "board.h"
#include "countdown.h"
#include "power.h"
#include "routines.h"
#include "sse.h"
#include "state.h"
#include "timing.h"
#include "http_util.h"
#include "fnv1a.h"

#define STATE_CHANGED_BIT BIT3

volatile uint32_t g_state_version = 1;
static EventGroupHandle_t g_state_evt = NULL;

/* ══════════════════════════════════════════════════════════════════════════
 * Change detection — a monotonic version counter (cheap, used to wake the
 * SSE task and as the SSE event id) plus a lazily-recomputed FNV-1a hash
 * over the cacheable subset of state (used as the HTTP ETag / cache
 * validator). See state.h for why these are two different things.
 *
 * s_cached_snap is the snapshot that produced s_cached_hash. Everything
 * that wants "the current state" — the hash, the JSON body, an SSE delta
 * — goes through ensure_fresh() below, so a single state_snapshot_build()
 * is shared by all of them until the next notify_bump_state() call.
 * ══════════════════════════════════════════════════════════════════════════ */

static state_snapshot_t  s_cached_snap;
static uint32_t          s_cached_hash = 0;
static bool              s_hash_dirty  = true;
static state_change_cb_t s_on_change   = NULL;

/* countdown_rem ticks once a second while a countdown is running. It's
 * deliberately the LAST field in state_snapshot_t (see state.h) so this
 * hashes everything EXCEPT it — hashing it would flip the ETag/hash every
 * second even though nothing a client can actually see over SSE changed
 * (countdown_rem isn't sent; the client derives it from cd_target_epoch).
 * Hashing it here previously defeated both the HTTP cache validator and
 * the whole point of the SSE delta encoder while any countdown was armed. */
#define STATE_HASH_LEN offsetof(state_snapshot_t, countdown_rem)

void state_register_on_change(state_change_cb_t cb) {
    s_on_change = cb;
}

void state_snapshot_build(state_snapshot_t *s) {
    memset(s, 0, sizeof(*s));
    /* memset matters for two reasons, not just tz[]'s missing NUL
     * guarantee from strncpy: routine_ids[] is only partially filled
     * (routine_count..11 stay untouched) and that unused tail sits
     * inside STATE_HASH_LEN. Leftover stack garbage there would make
     * state_get_hash() non-deterministic between otherwise-identical
     * states. Do not remove this in the name of trimming a memset. */
    time_t now = time(NULL);
    auto_off_t ao = relay_get_auto_off();

    s->relay                = relay_get() ? 1 : 0;
    s->led                  = led_get() ? 1 : 0;
    s->led_mode              = led_get_mode();
    s->boot_mode             = relay_get_boot_behavior();
    s->time_ok               = (now > TIME_VALID_THRESHOLD) ? 1 : 0;
    s->power_save_disabled   = power_save_is_disabled() ? 1 : 0;
    s->countdown_active          = countdown_is_active() ? 1 : 0;
    s->countdown_total           = countdown_get_total();
    s->cd_target_epoch           = countdown_get_target_epoch();
    s->routine_active_mask  = g_routine_active_mask;
    s->auto_off_armed        = ao.enabled ? 1 : 0;
    s->auto_off_h            = ao.hour;
    s->auto_off_m            = ao.minute;
    s->auto_off_s            = ao.second;

    s->routine_count = 0;
    for (int i = 0; i < ROUTINES_MAX; i++) {
        if (routine_at(i)) {
            s->routine_ids[s->routine_count] = (uint8_t)i;
            s->routine_count++;
        }
    }

    strncpy(s->tz, timing_get_timezone(), TZ_MAX_LEN - 1);

    s->sse_enabled = sse_is_enabled() ? 1 : 0;

    s->countdown_rem = countdown_get_remaining();  /* last: see STATE_HASH_LEN */
}

/* notify_bump_state — the single choke point every mutator must call.
 * Bumps the version counter (wakes any blocked SSE stream via
 * STATE_CHANGED_BIT) and marks the hash dirty for lazy recomputation.
 * Deliberately does NOT recompute the hash here: notify_bump_state() is
 * called from several different tasks (actuator, routines, countdown,
 * timing), and forcing a snapshot_build() on all of them serializes work
 * that the next state_get_hash() caller can do once, lazily. */
void notify_bump_state(void) {
    g_state_version++;
    s_hash_dirty = true;
    if (g_state_evt)
        xEventGroupSetBits(g_state_evt, STATE_CHANGED_BIT);
    if (s_on_change) s_on_change();
}

/* Shared gate: rebuilds s_cached_snap/s_cached_hash together, once, the
 * first time either is asked for after a notify_bump_state(). Every
 * subsequent ask before the next change is free. */
static void ensure_fresh(void) {
    if (s_hash_dirty) {
        state_snapshot_build(&s_cached_snap);
        s_cached_hash = fnv1a_hash(&s_cached_snap, STATE_HASH_LEN);
        s_hash_dirty  = false;
    }
}

uint32_t state_get_hash(void) {
    ensure_fresh();
    return s_cached_hash;
}

const state_snapshot_t *state_get_snapshot(void) {
    ensure_fresh();
    return &s_cached_snap;
}

/* The wire payload for GET /api/v1/state. Most field names are unchanged
 * from the original /state response — the frontend's syncState()/
 * loadState() parse these keys and mostly don't need to change. Two
 * exceptions, both intentional: "power_save" is new, and "auto_off_armed"
 * (a bare bool) has become "auto_off" (an object) so the UI can show what
 * it's actually configured to do, not just whether it's on. uptime is
 * included for the UI but intentionally NOT part of the hash above (see
 * state.h). SSE pushes use state_build_ssedata()'s binary encoding
 * instead of this JSON body. */
size_t state_build_json(char *buf, size_t buflen) {
    const state_snapshot_t *snap = state_get_snapshot();

    int n = snprintf(buf, buflen,
        "{\"relay\":%s,\"led\":%s,\"countdown\":%s,\"countdown_rem\":%lu,"
        "\"countdown_total\":%lu,\"cd_target_epoch\":%lu,"
        "\"boot_mode\":%d,\"led_mode\":%d,"
        "\"routine_mask\":%d,"
        "\"auto_off\":{\"armed\":%s,\"h\":%d,\"m\":%d,\"s\":%d},"
        "\"sse_enabled\":%s}",
        BOOL_STR(snap->relay),
        BOOL_STR(snap->led),
        BOOL_STR(snap->countdown_active),
        (unsigned long)snap->countdown_rem,
        (unsigned long)snap->countdown_total,
        (unsigned long)snap->cd_target_epoch,
        snap->boot_mode,
        snap->led_mode,
        snap->routine_active_mask,
        BOOL_STR(snap->auto_off_armed), snap->auto_off_h, snap->auto_off_m, snap->auto_off_s,
        BOOL_STR(snap->sse_enabled));

    return snprintf_guard(n, buflen);
}

bool state_wait_for_change(uint32_t last_hash, TickType_t ticks) {
    if (!g_state_evt) return false;
    EventBits_t bits = xEventGroupWaitBits(g_state_evt, STATE_CHANGED_BIT,
                                           pdTRUE, pdFALSE, ticks);
    uint32_t h = state_get_hash();
    return (bits & STATE_CHANGED_BIT) && h != last_hash;
}

void state_init(void) {
    g_state_evt = xEventGroupCreate();
}

/* ══════════════════════════════════════════════════════════════════════════
 * SSE data encoding — single-token base64-encoded binary for minimum traffic.
 *
 * Full state (13 bytes → 18 base64 chars):
 *   f<base64>   e.g. f4YGQAA... (18 chars)
 *
 *   Byte 0: [relay:1][led:1][cd_active:1][time_ok:1][boot_mode:2][spare:2]
 *   Byte 1: [led_mode:7][power_save_disabled:1]
 *   Bytes 2-5:  cd_target_epoch (uint32, big-endian)
 *   Bytes 6-8:  cd_total (uint24, big-endian; 0 when inactive)
 *   Byte 9: [routine_active_mask:2][auto_off_armed:1][auto_off_h:5]
 *   Byte 10: auto_off_m
 *   Byte 11: auto_off_s
 *   Byte 12: sse_enabled (0/1)
 *
 * Delta:
 *   d<base64>   e.g. dAQB (relay toggled on: mask 0x0001, val 01)
 *
 *   Bytes 0-1:  bitmask (big-endian, bit0=relay...)
 *   Followed by one value per SET bit, in bit order — each field gets
 *   its own byte(s), none are packed together:
 *     bit 0(relay), 1(led), 2(cd_active)          → 1 byte each
 *     bit 3(cd_target_epoch)                      → 4 bytes (uint32, big-endian)
 *     bit 4(cd_total)                              → 3 bytes (uint24, big-endian)
 *     bit 5(time_ok), 6(boot), 7(led_mode)         → 1 byte each
 *     bit 8(power_save), 9(rmask)                  → 1 byte each
 *     bit 10(armed), 11(aoff_h), 12(aoff_m), 13(aoff_s) → 1 byte each
 *     bit 14(sse_enabled)                          → 1 byte
 *
 * e.g. relay + led toggling together costs mask(2) + 1 + 1 = 4 bytes, not
 * a shared 1 — worth knowing if you're reasoning about worst-case delta
 * size (max 15 fields × their byte widths + 2-byte mask = 22 bytes).
 *
 * Countdown remaining time is NOT sent — the client derives it from
 * cd_target_epoch and its own clock. This eliminates the 60 deltas/min
 * that previously carried the ticking countdown_rem.
 * ══════════════════════════════════════════════════════════════════════════ */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_enc(const uint8_t *in, size_t inlen, char *out) {
    size_t i = 0;
    while (i + 3 <= inlen) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        *out++ = B64[(v >> 18) & 0x3f];
        *out++ = B64[(v >> 12) & 0x3f];
        *out++ = B64[(v >> 6) & 0x3f];
        *out++ = B64[v & 0x3f];
        i += 3;
    }
    if (i < inlen) {
        size_t rem = inlen - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        *out++ = B64[(v >> 18) & 0x3f];
        *out++ = B64[(v >> 12) & 0x3f];
        if (rem > 1) *out++ = B64[(v >> 6) & 0x3f];
        /* no padding */
    }
}

/* Encode a full snapshot into the 13-byte binary format. */
static void snap_encode_bin(const state_snapshot_t *s, uint8_t out[13]) {
    out[0] = (uint8_t)(
        ((s->relay              & 1) << 7) |
        ((s->led                & 1) << 6) |
        ((s->countdown_active   & 1) << 5) |
        ((s->time_ok            & 1) << 4) |
        ((s->boot_mode          & 3) << 2)
    );
    out[1] = (uint8_t)(
        ((s->led_mode & 0x7f) << 1) |
        ((!s->power_save_disabled & 1))
    );
    out[2]  = (uint8_t)(s->cd_target_epoch >> 24);
    out[3]  = (uint8_t)(s->cd_target_epoch >> 16);
    out[4]  = (uint8_t)(s->cd_target_epoch >> 8);
    out[5]  = (uint8_t)(s->cd_target_epoch);
    out[6]  = (uint8_t)(s->countdown_total >> 16);
    out[7]  = (uint8_t)(s->countdown_total >> 8);
    out[8]  = (uint8_t)(s->countdown_total);
    out[9]  = (uint8_t)(
        ((s->routine_active_mask & 3) << 6) |
        ((s->auto_off_armed      & 1) << 5) |
        ((s->auto_off_h          & 0x1f) << 0)
    );
    out[10] = s->auto_off_m;
    out[11] = s->auto_off_s;
    out[12] = s->sse_enabled;
}

/* Build the SSE data line — a single token: f<base64> for full, d<base64>
 * for delta.  Returns bytes written (including NUL), 0 on truncation. */
size_t state_build_ssedata(char *buf, size_t buflen,
                            const state_snapshot_t *prev,
                            const state_snapshot_t *cur) {
    if (buflen < 3) return 0;

    if (!prev) {
        /* Full state: 13 bytes → 18 base64 chars */
        if (buflen < 20) return 0;  /* f + 18 + NUL */
        uint8_t bin[13];
        snap_encode_bin(cur, bin);
        buf[0] = 'f';
        base64_enc(bin, 13, buf + 1);
        buf[19] = '\0';
        return 19;
    }

    /* Delta: [mask_hi][mask_lo][changed-field values...]. Built directly
     * into one fixed buffer — mask is filled in after the loop, once its
     * own length (len) is known — instead of assembling values into a
     * separate array and memcpy-ing them into a second, variable-length
     * one. One buffer, no VLA, no copy. */
    uint16_t mask = 0;
    uint8_t  delta[2 + 64];
    size_t   len = 2;   /* first 2 bytes reserved for the mask */

#define CHECK_BIT(bit, val, sz) do { \
    mask |= (uint16_t)(bit); \
    for (int _i = (int)(sz) - 1; _i >= 0; _i--) delta[len++] = (uint8_t)((uint64_t)(val) >> (_i * 8)); \
} while (0)

    if (prev->relay               != cur->relay)               { CHECK_BIT(0x0001, cur->relay, 1); }
    if (prev->led                 != cur->led)                 { CHECK_BIT(0x0002, cur->led, 1); }
    if (prev->countdown_active    != cur->countdown_active)    { CHECK_BIT(0x0004, cur->countdown_active, 1); }
    if (prev->cd_target_epoch     != cur->cd_target_epoch)     { CHECK_BIT(0x0008, cur->cd_target_epoch, 4); }
    if (prev->countdown_total     != cur->countdown_total)     { CHECK_BIT(0x0010, cur->countdown_total, 3); }
    if (prev->time_ok             != cur->time_ok)             { CHECK_BIT(0x0020, cur->time_ok, 1); }
    if (prev->boot_mode           != cur->boot_mode)           { CHECK_BIT(0x0040, cur->boot_mode, 1); }
    if (prev->led_mode            != cur->led_mode)            { CHECK_BIT(0x0080, cur->led_mode, 1); }
    if (prev->power_save_disabled != cur->power_save_disabled) { CHECK_BIT(0x0100, !cur->power_save_disabled, 1); }
    if (prev->routine_active_mask != cur->routine_active_mask) { CHECK_BIT(0x0200, cur->routine_active_mask, 1); }
    if (prev->auto_off_armed      != cur->auto_off_armed)      { CHECK_BIT(0x0400, cur->auto_off_armed, 1); }
    if (prev->auto_off_h          != cur->auto_off_h)          { CHECK_BIT(0x0800, cur->auto_off_h, 1); }
    if (prev->auto_off_m          != cur->auto_off_m)          { CHECK_BIT(0x1000, cur->auto_off_m, 1); }
    if (prev->auto_off_s          != cur->auto_off_s)          { CHECK_BIT(0x2000, cur->auto_off_s, 1); }
    if (prev->sse_enabled         != cur->sse_enabled)         { CHECK_BIT(0x4000, cur->sse_enabled, 1); }

#undef CHECK_BIT

    if (len == 2) return 0;  /* nothing changed */

    delta[0] = (uint8_t)(mask >> 8);
    delta[1] = (uint8_t)(mask);

    /* base64_enc() is unpadded: a 1-byte remainder costs 2 chars, a
     * 2-byte remainder costs 3 — never a full 4. ceil(len/3)*4 assumes
     * padding and overshoots by 1-2 chars whenever len isn't a multiple
     * of 3, which the fixed 12-byte full-state path never hits but this
     * variable-length delta path does. ceil(len*4/3), i.e. (len*4+2)/3,
     * matches base64_enc()'s actual unpadded output length. */
    size_t b64len = (len * 4 + 2) / 3;
    if (buflen < 1 + b64len + 1) return 0;  /* d + base64 + NUL */
    buf[0] = 'd';
    base64_enc(delta, len, buf + 1);
    buf[1 + b64len] = '\0';
    return 1 + b64len;
}