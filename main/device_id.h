#ifndef DEVICE_ID_H
#define DEVICE_ID_H

/* Short alphanumeric device ID derived from the STA MAC last 3 bytes
 * (e.g. "A1B2C3"). Stable across reboots and factory resets (MAC is
 * immutable). Buffer must be at least DEVICE_ID_MIN_LEN (7) bytes. */
#define DEVICE_ID_MIN_LEN 7

/* Fills buf with the device ID (uppercase hex, NUL-terminated). */
void device_id(char *buf, size_t buflen);

#endif /* DEVICE_ID_H */
