#define _POSIX_C_SOURCE 199309L
#include "update_common.h"
#include <time.h>

/* ============ module-local framework model (externs + structs) ============ */
/* In-memory firmware image (begin/end/cap triplet of the OEM std::vector<char>). */
typedef struct c2000_file_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} c2000_file_buf;

/* Opaque client wrapping a serial transport via a small vtable.
   In the OEM, cl->serial is *(client+8) and the app image path is the
   std::string at client+0x10. */
typedef struct c2000_client c2000_client;
/* Returns the std::string application-image path stored at client+0x10. */
const char *c2000_app_image_path(c2000_client *cl);
/* Serial-transport vtable thunks (slots observed in the OEM):
     +0x10 putc, +0x18 write, +0x40 read, +0x60 lock, +0x68 unlock, +0x70 flush. */
void cl_putc(c2000_client *cl, int byte);                 /* vtable +0x10 */
void cl_write(c2000_client *cl, const uint8_t *buf, size_t len); /* vtable +0x18 */
int  cl_read(c2000_client *cl, char *out, int timeout_ms); /* vtable +0x40; 0 on success, !=0 on timeout */
void cl_serial_lock(c2000_client *cl);                    /* vtable +0x60 */
void cl_serial_unlock(c2000_client *cl);                  /* vtable +0x68 */
void cl_serial_flush(c2000_client *cl);                   /* vtable +0x70 */
/* Progress-report callback object (the OEM passes a functor pointer whose
   +0x18 slot is invoked with a uint64 byte count when +0x10 is non-null). */
typedef struct progress_cb progress_cb;
void progress_report(progress_cb *prog, uint64_t bytes_done);
/* Slurp an entire file into a heap buffer (models FUN_00123b30 reading the
   firmware image into a std::vector<char>; throws/aborts on open failure). */
void read_file_to_buf(c2000_file_buf *out, const char *path);
void file_buf_free(c2000_file_buf *buf);
/* ========================================================================== */

/*
 * c2000_update_client.c — TI TMS320F40049C motor-controller flashing
 *
 * Behaviour-oriented reconstruction of VanMoof's update service C2000 flasher.
 * Source path embedded in the OEM image:
 *   devices/main/update/src/c2000_update_client.cpp
 *
 * Flow:
 *   1. flash_motor_controller(): GPIO pulse-reset into the TI ROM bootloader,
 *      auto-baud detect ('A' handshake), stream the SCI kernel image with
 *      per-byte echo verification, re-detect baud, then stream the DFU
 *      application image (running 16-bit checksum, block framing, footer
 *      magic, status 0x1000).
 *   2. flash_kernel_stream(): byte-by-byte send of the SCI kernel with echo
 *      verify (300 ms per byte).
 *   3. flash_application_image(): DFU block protocol with running checksum.
 *
 * The "client" object (cl) wraps a serial transport via a small vtable.
 * Slot offsets observed in the OEM (vtable at *(cl->serial)):
 *     +0x10  putc(serial, byte)
 *     +0x18  write(serial, buf, len)
 *     +0x40  read(serial, *out_byte, timeout_ms)   -> 0 on success
 *     +0x60  serial_lock(serial)                   (open / acquire)
 *     +0x68  serial_unlock(serial)                 (close / release)
 *     +0x70  serial_flush(serial)
 * These are modelled by the cl_* helpers declared in externs.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* ---- OEM .rodata constants (verbatim) -------------------------------- */

/* GPIO sysfs base + the three reset lines. */
#define GPIO_SYSFS_DIR      "/sys/class/gpio"          /* 0x173c90 */
#define GPIO_RESET_12       "gpio12/value"             /* 0x173c80 */
#define GPIO_RESET_125      "gpio125/value"            /* 0x173cb0 */
#define GPIO_RESET_126      "gpio126/value"            /* 0x173cc0 */

/* SCI kernel image fed to the ROM bootloader. */
#define SCI_KERNEL_PATH     "/usr/bin/tms320f40049c_sci_kernel.bin"  /* 0x173d30 */

/*
 * The 10-byte DFU "ping" packet sent before the application image
 * (.rodata @0x173e08): e4 1b 00 00 00 01 01 00 1b e4
 *   header magic 0x1be4 (little-endian e4 1b), command 01, footer 0xe41b.
 */
static const uint8_t c2000_dfu_packet[10] = {
    0xe4, 0x1b, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x1b, 0xe4
};

/* ---- helpers --------------------------------------------------------- */

/* OEM 0x130da0-region: interruptible nanosleep wrapper. */
static void msleep_ns(long nsec)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = nsec;
    do {
        if (nanosleep(&ts, &ts) != -1)
            break;
    } while (errno == EINTR);
}

/* OEM helper: write a single decimal value + '\n' to a gpioNN/value file.
 * Returns 0 on success, -errno on failure (matching the OEM accumulator). */
static int gpio_write_value(const char *gpio_leaf, int value)
{
    char path[0x80];
    FILE *f;
    int rc = 0;

    /* OEM: __snprintf_chk(buf, 0x80, 1, 0x80, "%s/%s", GPIO_SYSFS_DIR, leaf) */
    snprintf(path, sizeof(path), "%s/%s", GPIO_SYSFS_DIR, gpio_leaf);

    f = fopen(path, "w");
    if (f == NULL)
        return -errno;

    /* OEM: __fprintf_chk(f, 1, "%d\n", value) */
    if (fprintf(f, "%d\n", value) < 0)
        rc = -errno;
    fclose(f);
    return rc;
}

/*
 * OEM 0x130ad0  FUN_00130ad0 — auto-baud detection.
 *
 * Up to 0x32 (50) attempts: sleep 100 ms, flush, send 'A' (0x41), wait up to
 * 300 ms for the bootloader to echo 'A'. Returns 0 once 'A' is seen, or -1
 * after 50 failed rounds.
 */
static int c2000_detect_baudrate(c2000_client *cl)
{
    int attempts = 0x32;
    char in = '\0';

    do {
        msleep_ns(100000000L);           /* 100 ms */

        cl_serial_flush(cl);
        cl_putc(cl, 0x41);               /* 'A' */
        (void)cl_read(cl, &in, 300);     /* 300 ms timeout */

        if (--attempts == 0 || in == 'A')
            break;
    } while (1);

    /* OEM returns -(in != 'A'): 0 on match, -1 otherwise. */
    return (in == 'A') ? 0 : -1;
}

/*
 * OEM 0x130490  FUN_00130490 — read one DFU response word ("checksum read").
 *
 * Reads a byte (timeout_ms), echoes back '-' (0x2d) as the ACK, reads a second
 * byte, echoes '-' again. Packs the two bytes little-endian into bits 31..16
 * and sets bit 0 as the "valid" flag:
 *     return (lo | (hi<<8)) << 16 | 1     on success
 *     return 0                            on timeout
 *
 * The caller uses (ret & 0xff) as a validity boolean and (ret >> 16) as the
 * 16-bit value.
 */
static uint32_t c2000_read_word(c2000_client *cl, int timeout_ms)
{
    uint8_t lo = 0, hi = 0;

    if (cl_read(cl, (char *)&lo, timeout_ms) != 0)
        return 0;
    cl_putc(cl, 0x2d);                   /* '-' ACK */

    hi = lo;                             /* OEM saves first byte before reusing slot */
    if (cl_read(cl, (char *)&hi, timeout_ms) != 0)
        return 0;
    cl_putc(cl, 0x2d);                   /* '-' ACK */

    return ((uint32_t)((uint16_t)((hi << 8) | lo)) << 16) | 1u;
}

/*
 * OEM 0x130320  c2000_flash_kernel_stream — stream the SCI kernel.
 *
 * Sends every byte of the kernel buffer. For each byte except the last two,
 * waits up to 300 ms for the bootloader to echo the byte back and compares.
 * (The final two bytes are sent without echo verification — the ROM loader
 * stops echoing once it begins executing the freshly loaded kernel.)
 *
 * Returns 0 on success, -1 on timeout or verify mismatch.
 */
int c2000_flash_kernel_stream(c2000_client *cl, const uint8_t *buf, size_t len,
                              progress_cb *prog)
{
    size_t i;
    size_t sent = 0;
    char echo;

    if (len == 0)
        return -1;

    for (i = 0; i < len; i++) {
        cl_putc(cl, buf[i]);

        if (prog != NULL) {
            sent++;
            progress_report(prog, sent);
        }

        /* All but the last two bytes are echo-verified. */
        if (i < len - 2) {
            echo = '\0';
            if (cl_read(cl, &echo, 300) != 0) {
                common_logf("devices/main/update/src/c2000_update_client.cpp",
                            0x87, LOG_WARN,
                            "Timeout failure during kernel update");
                return -1;
            }
            if ((char)buf[i] != echo) {
                common_logf("devices/main/update/src/c2000_update_client.cpp",
                            0x8b, LOG_WARN,
                            "Verify failure in update kernel");
                return -1;
            }
        }
    }
    return 0;
}

/*
 * OEM 0x130580  c2000_flash_application_image — DFU application download.
 *
 * Protocol (TI C2000 serial flash kernel "DFU" framing):
 *  - Send the 10-byte ping packet, expect a '-' (0x2d) ACK within 300 ms.
 *  - Loop over blocks. Each block is prefixed (in the image) by a 6-byte
 *    header: a 16-bit block-size word (count of 16-bit data words), then 4
 *    more header bytes. A block-size of 0 terminates the data phase.
 *      * Stream 2*size data bytes from the image. Every 256 sent bytes,
 *        request an intermediate checksum word (FUN_00130490, 100 ms) and
 *        verify the low validity flag.
 *      * After each block's data, request the block checksum (100 ms) and
 *        compare against the locally accumulated 16-bit running sum.
 *      * Send the next 6-byte header and fold its bytes into the running sum.
 *  - After the terminating zero-size block, read the trailer words:
 *      header  word == 0x1be4  (1000 ms)
 *      length  word valid      (100 ms)
 *      command word valid      (100 ms)
 *      status  word valid      (100 ms)  -> low 16 bits must equal 0x1000
 *      'len-2'/2 data words     (100 ms each)
 *      checksum word valid     (100 ms)
 *      footer  word == 0xe41b  (100 ms)  -> then send final '-' (0x2d)
 *
 * Returns 0 on full success, -1 on any framing/checksum/status error.
 *
 * The running checksum is a plain 16-bit modular sum of every data byte and
 * every header byte (matching the OEM `& 0xffff` masking).
 */
int c2000_flash_application_image(c2000_client *cl, const uint8_t *img, size_t img_len,
                                  progress_cb *prog)
{
    /* 6-byte block header; first word initialised to 0x000b per the OEM
     * (local_18=0x0b, the rest zero) so the first iteration streams a block. */
    uint8_t hdr[6] = { 0x0b, 0, 0, 0, 0, 0 };
    uint16_t block_size;
    uint32_t checksum = 0;          /* 16-bit running sum (kept in a u32, masked) */
    size_t pos = 0;
    char ack;
    uint32_t w;

    /* Send DFU ping packet, expect '-' ACK. */
    cl_write(cl, c2000_dfu_packet, 10);
    ack = '\0';
    if (cl_read(cl, &ack, 300) != 0 || ack != '-') {
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0xa7, LOG_WARN, "Failed sending DFU packet");
        return -1;
    }

    block_size = (uint16_t)(hdr[0] | (hdr[1] << 8));

    do {
        uint32_t i = 0;                 /* byte counter for this block (2*size) */
        uint32_t limit = (uint32_t)block_size << 1;

        if (block_size != 0) {
            while (i < limit) {
                uint8_t b;

                /* Every 256 streamed bytes (and not at the very start),
                 * flush and validate an intermediate checksum. */
                if (i != 0 && (i & 0xff) == 0) {
                    cl_serial_flush(cl);
                    w = c2000_read_word(cl, 100);
                    if ((w & 0xff) == 0) {
                        common_logf("devices/main/update/src/c2000_update_client.cpp",
                                    0xb6, LOG_WARN,
                                    "Invalid checksum (1) in application update");
                        return -1;
                    }
                    checksum = w >> 0x10;   /* resync to device's running sum */
                }

                b = img[pos++];
                cl_putc(cl, b);
                if (prog != NULL)
                    progress_report(prog, pos);

                checksum = (checksum + b) & 0xffff;
                i++;
            }
        }

        /* End of block: validate the block checksum word against ours. */
        cl_serial_flush(cl);
        w = c2000_read_word(cl, 100);
        if ((w & 0xff) == 0 || checksum != (w >> 0x10)) {
            common_logf("devices/main/update/src/c2000_update_client.cpp",
                        200, LOG_WARN,
                        "Invalid checksum (2) in application update");
            return -1;
        }

        /* Read the next 6-byte header from the image and send it. */
        hdr[0] = img[pos + 0];
        hdr[1] = img[pos + 1];
        hdr[2] = img[pos + 2];
        hdr[3] = img[pos + 3];
        hdr[4] = img[pos + 4];
        hdr[5] = img[pos + 5];
        cl_write(cl, hdr, 6);
        if (prog != NULL)
            progress_report(prog, pos);
        pos += 6;

        block_size = (uint16_t)(hdr[0] | (hdr[1] << 8));

        /* Fold the header bytes into the running sum (OEM order:
         * hdr[3]+hdr[2]+hdr[1]+hdr[0]+hdr[5]+hdr[4]). */
        checksum = (checksum + hdr[3] + hdr[2] + hdr[1] + hdr[0]
                             + hdr[5] + hdr[4]) & 0xffff;

    } while (block_size != 0 && pos <= img_len);

    /* ---- trailer ---- */

    /* Header word: 0x1be4 (1000 ms). */
    w = c2000_read_word(cl, 1000);
    if ((w & 0xff) == 0 || (w >> 0x10) != 0x1be4) {
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0xe8, LOG_WARN, "Invalid header in application update");
        return -1;
    }

    /* Length word (100 ms). */
    w = c2000_read_word(cl, 100);
    if ((w & 0xff) == 0) {
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0xf0, LOG_WARN, "Invalid length in application update");
        return -1;
    }
    {
        uint32_t len_word = w >> 0x10;

        /* Command word (100 ms). */
        if ((c2000_read_word(cl, 100) & 0xff) == 0) {
            common_logf("devices/main/update/src/c2000_update_client.cpp",
                        0xf8, LOG_WARN, "Invalid command in application update");
            return -1;
        }

        /* Status word (100 ms). */
        w = c2000_read_word(cl, 100);
        if ((w & 0xff) == 0) {
            common_logf("devices/main/update/src/c2000_update_client.cpp",
                        0xfe, LOG_WARN, "Invalid status in application update");
            return -1;
        }
        {
            uint32_t status_word = (w >> 0x10) & 0xffff;

            /* Drain (len-2)/2 trailing data words. */
            if (len_word > 2) {
                uint32_t ndata = (len_word - 2) >> 1;
                uint32_t k;
                for (k = 0; k < ndata; k++) {
                    if ((c2000_read_word(cl, 100) & 0xff) == 0) {
                        common_logf("devices/main/update/src/c2000_update_client.cpp",
                                    0x107, LOG_WARN,
                                    "Invalid data in application update");
                        return -1;
                    }
                }
            }

            /* Trailer checksum word (100 ms). */
            if ((c2000_read_word(cl, 100) & 0xff) == 0) {
                common_logf("devices/main/update/src/c2000_update_client.cpp",
                            0x10f, LOG_WARN,
                            "Invalid checksum (3) in application update");
                return -1;
            }

            /* Footer word: 0xe41b, validity bit set (100 ms). */
            w = c2000_read_word(cl, 100);
            if ((w >> 0x10) != 0xe41b || ((w ^ 1) & 0xff) != 0) {
                common_logf("devices/main/update/src/c2000_update_client.cpp",
                            0x116, LOG_WARN,
                            "Invalid footer '%d' in application update");
                return -1;
            }

            /* Final ACK. */
            cl_putc(cl, 0x2d);           /* '-' */

            if (status_word != 0x1000) {
                common_logf("devices/main/update/src/c2000_update_client.cpp",
                            0x11e, LOG_WARN,
                            "Flashing failed in application update");
                return -1;
            }
        }
    }

    return 0;
}

/*
 * OEM 0x130bd0  c2000_flash_motor_controller — top-level flash sequence.
 *
 * cl->serial is the serial transport; cl->app_image_path (cl+0x10) is the
 * std::string path to the application image.
 *
 * Returns 0 on success, -1 on failure.
 */
int c2000_flash_motor_controller(c2000_client *cl)
{
    int gpio_rc = 0;       /* uVar9 accumulator (gpio12 + gpio125) */
    int gpio_rc2 = 0;      /* uVar8 accumulator (gpio126 + gpio12 deassert) */
    int rc;

    /* Acquire the serial port (vtable +0x60). */
    cl_serial_lock(cl);

    /* Pulse-reset the C2000 into its ROM bootloader:
     *   gpio12  = 1, gpio125 = 1, gpio126 = 0, sleep 20 ms, gpio12 = 0. */
    gpio_rc  = gpio_write_value(GPIO_RESET_12,  1);
    gpio_rc |= gpio_write_value(GPIO_RESET_125, 1);
    gpio_rc2 = gpio_write_value(GPIO_RESET_126, 0);

    msleep_ns(20000000L);                /* 20 ms */

    gpio_rc2 |= gpio_write_value(GPIO_RESET_12, 0);

    if ((gpio_rc | gpio_rc2) != 0) {
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0x39, LOG_ERR, "Unable to reset motor controller");
        /* OEM: logs but proceeds. */
    }

    /* ---- kernel phase ---- */
    common_logf("devices/main/update/src/c2000_update_client.cpp",
                0x3c, LOG_INFO, "Determine baud rate");
    if (c2000_detect_baudrate(cl) == -1) {
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0x3f, LOG_WARN, "Auto baudrate detection failed.");
        cl_serial_unlock(cl);
        return -1;
    }

    {
        c2000_file_buf kernel;
        progress_cb *prog = NULL;        /* OEM passes a progress callback object */

        /* Load the SCI kernel image. */
        read_file_to_buf(&kernel, SCI_KERNEL_PATH);
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0x47, LOG_INFO, "Flashing kernel file '%s'", SCI_KERNEL_PATH);

        rc = c2000_flash_kernel_stream(cl, kernel.data, kernel.len, prog);
        file_buf_free(&kernel);

        if (rc != 0) {
            cl_serial_unlock(cl);
            return rc;
        }

        /* ---- application phase ---- */
        common_logf("devices/main/update/src/c2000_update_client.cpp",
                    0x55, LOG_INFO, "Determine baud rate");
        if (c2000_detect_baudrate(cl) != 0) {
            common_logf("devices/main/update/src/c2000_update_client.cpp",
                        0x58, LOG_WARN, "Auto baudrate detection failed.");
            cl_serial_unlock(cl);
            return -1;
        }

        {
            c2000_file_buf app;
            const char *app_path = c2000_app_image_path(cl); /* cl+0x10 */

            common_logf("devices/main/update/src/c2000_update_client.cpp",
                        0x5e, LOG_INFO, "Flashing application file '%s'", app_path);

            read_file_to_buf(&app, app_path);
            rc = c2000_flash_application_image(cl, app.data, app.len, prog);
            file_buf_free(&app);

            /* OEM releases the port either way (vtable +0x68). */
            cl_serial_unlock(cl);
        }
    }

    return rc;
}