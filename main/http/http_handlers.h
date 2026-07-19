#ifndef HTTP_HANDLERS_H
#define HTTP_HANDLERS_H

#include "esp_http_server.h"

/* ── http_handlers_state.c ───────────────────────────────────────────────
 * Device identity, observable state, diagnostics, and lifecycle actions.
 */
esp_err_t get_state(httpd_req_t *req);          /* GET  /api/v1/state            */
esp_err_t get_state_stream(httpd_req_t *req);   /* GET  /api/v1/state/stream     */
esp_err_t get_system(httpd_req_t *req);         /* GET  /api/v1/system           */
esp_err_t post_system_reset(httpd_req_t *req);         /* POST /api/v1/system/reset        */
esp_err_t post_system_factory_reset(httpd_req_t *req); /* POST /api/v1/system/factory-reset */
esp_err_t get_ping(httpd_req_t *req);                  /* GET  /api/v1/ping                */

/* ── http_handlers_control.c ─────────────────────────────────────────────
 * Actuation and device configuration.
 */
esp_err_t post_relay(httpd_req_t *req);         /* POST   /api/v1/relay          */
esp_err_t put_boot(httpd_req_t *req);           /* PUT    /api/v1/config/boot    */
esp_err_t put_led(httpd_req_t *req);            /* PUT    /api/v1/config/led     */
esp_err_t put_tz(httpd_req_t *req);             /* PUT    /api/v1/config/timezone*/
esp_err_t put_power_save(httpd_req_t *req);     /* PUT    /api/v1/config/power-save */
esp_err_t put_auto_off(httpd_req_t *req);       /* PUT    /api/v1/config/auto-off */
esp_err_t post_timer(httpd_req_t *req);         /* POST   /api/v1/timer          */
esp_err_t delete_timer(httpd_req_t *req);       /* DELETE /api/v1/timer          */

/* ── http_handlers_routines.c ────────────────────────────────────────────
 * Scheduled/circulating routines (CRUD).
 */
esp_err_t get_routines(httpd_req_t *req);       /* GET    /api/v1/routines[?id=] */
esp_err_t post_routines(httpd_req_t *req);      /* POST   /api/v1/routines       */
esp_err_t delete_routine(httpd_req_t *req);     /* DELETE /api/v1/routines?id=   */

#endif /* HTTP_HANDLERS_H */
