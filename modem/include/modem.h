#ifndef MODEM_H
#define MODEM_H

/*
 * modem.h -- shared declarations for the VanMoof S5 nRF9160 modem application
 * reconstruction.
 *
 * Target: Nordic nRF9160 (Cortex-M33), Zephyr RTOS + nRF Connect SDK, MCUboot
 * image (base 0x0). Reconstructed from the OEM image (build/ida-equivalent
 * Ghidra decompile). This is REFERENCE C: the real build is a Zephyr / nRF
 * Connect SDK application, so the Zephyr kernel + driver APIs and the nRF
 * modem/LTE/location/SMS/DFU libraries are VENDOR -- they are the actual SDK
 * headers, modelled here as opaque externs so the VanMoof application logic
 * reads cleanly. Only the VanMoof application functions (in src/) are
 * reconstructed; the ~1,300 Zephyr/nRF/mbedTLS/newlib functions are deferred.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Zephyr core (modelled; real defs in zephyr/kernel.h etc.) ---- */
typedef struct k_thread   k_thread_t;
typedef struct k_msgq     k_msgq_t;
typedef struct k_sem      k_sem_t;
typedef struct k_pipe     k_pipe_t;
typedef struct k_poll_event k_poll_event_t;
typedef struct device     device_t;        /* Zephyr device instance */
typedef struct ring_buf   ring_buf_t;

#define K_NO_WAIT   (0)
#define K_FOREVER   (-1)

extern int  k_msgq_put(k_msgq_t *q, const void *data, void *fmt, int timeout);
extern int  k_msgq_get(k_msgq_t *q, void *data, int timeout);
extern int  k_pipe_get(k_pipe_t *p, void *data, size_t n, size_t *got, size_t min, int timeout);
extern int  k_pipe_put(k_pipe_t *p, const void *data, size_t n, size_t *put, size_t min, int timeout);
extern int  k_poll(k_poll_event_t *events, int num, int timeout);
extern void k_sem_give(k_sem_t *s);
extern int  k_sem_take(k_sem_t *s, int timeout);
extern void k_sleep(int ms);

/* Zephyr logging (modelled as printf-likes; real macros in zephyr/logging/log.h) */
extern void LOG_ERR(const char *fmt, ...);
extern void LOG_WRN(const char *fmt, ...);
extern void LOG_INF(const char *fmt, ...);
extern void LOG_DBG(const char *fmt, ...);

/* Zephyr SPI driver (the nRF9160 is SPI slave -- spi_nrfx_spis) */
struct spi_buf      { void *buf; size_t len; };
struct spi_buf_set  { const struct spi_buf *buffers; size_t count; };
struct spi_config   { uint32_t frequency; uint16_t operation; uint16_t slave; const device_t *ops; };
typedef int  (*spi_transceive_t)(const device_t *, uint32_t);
typedef int  (*spi_async_t)(const device_t *, struct spi_config *,
                            const struct spi_buf_set *, const struct spi_buf_set *);

/* Zephyr ring buffer + helpers used by the SPI/COBS path */
extern int  ring_buf_put_claim(ring_buf_t *rb, uint8_t *data, int n);
extern bool ring_buf_is_empty(ring_buf_t *rb);
extern bool ring_buf_is_ready(ring_buf_t *rb);
extern void ring_buf_reset(ring_buf_t *rb, void *buf, size_t size);

/* VanMoof helpers reconstructed alongside (CRC16 + COBS + SPI framing) */
extern uint16_t crc16_reflect(uint16_t seed, const void *data, size_t len);   /* FUN_0003404c */
extern int      cobs_decode(ring_buf_t *rb, uint8_t *out, size_t cap, uint8_t *tmp); /* FUN_0003e162 */
extern int      spi_slave_tx_ready(void *frame, int has_payload);             /* FUN_00033eb4 */

/* ---- nRF Connect SDK / modem library (vendor) ---- */
extern int  nrf_modem_at_printf(const char *fmt, ...);
extern int  nrf_modem_at_cmd(void *buf, size_t len, const char *fmt, ...);
extern int  lte_lc_connect(void);
extern int  lte_lc_init(void);

/* ---- subscribe table entry (the modem/* SPI publish/subscribe map) ---- */
struct sub_entry { uint16_t channel_id; int (*handler)(void *, void *); void *ctx; };

#endif /* MODEM_H */
