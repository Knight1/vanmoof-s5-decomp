/*
 * c2000_flash.c — VanMoof S5 motor-controller (TI TMS320F280049C) SCI flasher
 * library (pkg vmxs5-embedded-motor-update-lib), reconstructed behaviour-faithful
 * from the OEM decompilation (program "motor_update_example", base 0x100000).
 *
 * Reproduces the VanMoof flashing logic:
 *   - set_boot_mode  0x1071c0  (board GPIO 12/125/126 boot-mode strobe)
 *   - autobaud       0x107e60  (TI C2000 SCI autobaud: 'A' lock)
 *   - send_kernel    0x107850  (byte-by-byte echo upload of the SCI flash kernel)
 *   - flash_app      0x107ae0  (DFU block-streaming application programmer)
 *
 * The serial transport (serial_port.cpp termios), the sysfs GPIO (gpio.cpp) and
 * the std::ifstream are VENDOR; modelled here over POSIX so the protocol logic is
 * self-contained and reads clean. The wire format (autobaud byte, DFU packet,
 * 0x2d ping, 16-bit checksum, 0x1be4/0x1000/0xe41b status words) is reproduced
 * verbatim from the binary.
 */
#include "motor_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>

/* ------------------------------------------------------------------------ */
/* last-error (models the OEM std::runtime_error thrown on failure)          */
/* ------------------------------------------------------------------------ */
static const char *g_last_error = "";
const char *c2000_last_error(void) { return g_last_error; }
static int fail(const char *msg) { g_last_error = msg; return -1; }

/* ------------------------------------------------------------------------ */
/* opaque objects                                                            */
/* ------------------------------------------------------------------------ */
struct c2000_serial  { int fd; };
struct c2000_flasher { c2000_serial *serial; };   /* OEM holds it at +8 */
struct c2000_image   { uint8_t *data; size_t len; size_t pos; bool ok; };

/* ------------------------------------------------------------------------ */
/* serial transport (vendor serial_port.cpp, modelled over POSIX termios)    */
/*   vtable +0x10 write_byte, +0x18 write(buf,n), +0x40 read_byte(out,tmo),   */
/*   +0x70 flush.                                                            */
/* ------------------------------------------------------------------------ */
static void serial_write_byte(c2000_serial *s, uint8_t b)        /* +0x10 */
{
    (void)!write(s->fd, &b, 1);
}

static void serial_write(c2000_serial *s, const void *buf, size_t n) /* +0x18 */
{
    (void)!write(s->fd, buf, n);
}

static void serial_flush(c2000_serial *s)                        /* +0x70 */
{
    tcflush(s->fd, TCIFLUSH);
}

/* read one byte with a millisecond timeout; 0 on success, -1 on timeout. */
static int serial_read_byte(c2000_serial *s, uint8_t *out, int timeout_ms) /* +0x40 */
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(s->fd, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(s->fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return -1;
    return (read(s->fd, out, 1) == 1) ? 0 : -1;
}

/* read a 16-bit little-endian word (OEM FUN_001079b0). */
static uint16_t serial_read_word(c2000_flasher *f, int timeout_ms)
{
    uint8_t lo = 0, hi = 0;
    serial_read_byte(f->serial, &lo, timeout_ms);
    serial_read_byte(f->serial, &hi, timeout_ms);
    return (uint16_t)(lo | (hi << 8));
}

/* ------------------------------------------------------------------------ */
/* boot-mode GPIO (vendor gpio.cpp, modelled over sysfs)                      */
/* ------------------------------------------------------------------------ */
static void gpio_write_file(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(val, fp); fclose(fp); }
}

static void gpio_export_output(int pin)
{
    char p[64], v[16];
    snprintf(v, sizeof v, "%d", pin);
    gpio_write_file("/sys/class/gpio/export", v);
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", pin);
    gpio_write_file(p, "out");
}

static void gpio_set(int pin, int value)
{
    char p[64];
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", pin);
    gpio_write_file(p, value ? "1" : "0");
}

static void sleep_ns(long ns)
{
    struct timespec ts;
    ts.tv_sec  = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    while (nanosleep(&ts, &ts) == -1 && /* EINTR */ 1) {
        /* OEM retries while errno == EINTR(4); nanosleep updates ts. */
        break;
    }
}

/*
 * c2000_set_boot_mode — OEM 0x1071c0.
 * GPIO 12 is a strobe held high across the change then dropped; GPIO 125 is held
 * 1; GPIO 126 carries the mode (0 = enter SCI boot, 1 = run). 20 ms settle.
 */
void c2000_set_boot_mode(int mode)
{
    static bool inited;
    if (mode != 0 && mode != 1) {
        /* OEM throws std::runtime_error("Unsupported C2000 Boot Mode"). */
        g_last_error = "Unsupported C2000 Boot Mode";
        return;
    }
    if (!inited) {
        gpio_export_output(12);     /* 0xc  strobe */
        gpio_export_output(125);    /* 0x7d select (held 1) */
        gpio_export_output(126);    /* 0x7e mode    */
        inited = true;
    }
    gpio_set(12, 1);                /* strobe high */
    gpio_set(126, mode);           /* 0 = SCI boot, 1 = run */
    gpio_set(125, 1);
    sleep_ns(20000000L);           /* 20 ms */
    gpio_set(12, 0);               /* strobe low (latch) */
}

/* ------------------------------------------------------------------------ */
/* serial port + flasher + image lifecycle                                   */
/* ------------------------------------------------------------------------ */
c2000_serial *c2000_serial_open(const char *ttydev)              /* 0x109b90 */
{
    c2000_serial *s = (c2000_serial *)malloc(sizeof *s);
    struct termios tio;
    if (!s)
        return NULL;
    s->fd = open(ttydev, O_RDWR | O_NOCTTY);
    if (s->fd >= 0 && tcgetattr(s->fd, &tio) == 0) {
        cfmakeraw(&tio);                 /* 8N1 raw */
        cfsetispeed(&tio, B9600);        /* the DSP ROM autobauds to the host rate */
        cfsetospeed(&tio, B9600);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(s->fd, TCSANOW, &tio);
    }
    return s;
}

void c2000_serial_close(c2000_serial *s)
{
    if (s) {
        if (s->fd >= 0)
            close(s->fd);
        free(s);
    }
}

c2000_flasher *c2000_flasher_new(c2000_serial *s)               /* 0x107820 */
{
    c2000_flasher *f = (c2000_flasher *)malloc(sizeof *f);
    if (f)
        f->serial = s;
    return f;
}

void c2000_flasher_delete(c2000_flasher *f) { free(f); }

c2000_image *c2000_image_open(const char *path)                /* 0x105f20 (ifstream) */
{
    c2000_image *img = (c2000_image *)calloc(1, sizeof *img);
    FILE *fp;
    long n;
    if (!img)
        return NULL;
    fp = fopen(path, "rb");
    if (!fp)
        return img;                 /* img->ok stays false (stream badbit/failbit) */
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n > 0) {
        img->data = (uint8_t *)malloc((size_t)n);
        if (img->data && fread(img->data, 1, (size_t)n, fp) == (size_t)n) {
            img->len = (size_t)n;
            img->ok  = true;
        }
    }
    fclose(fp);
    return img;
}

bool c2000_image_ok(const c2000_image *img) { return img && img->ok; }

void c2000_image_close(c2000_image *img)
{
    if (img) {
        free(img->data);
        free(img);
    }
}

/* read up to n bytes from the image (ifstream read); returns bytes read. */
static size_t image_read(c2000_image *img, void *buf, size_t n)
{
    size_t avail = img->len - img->pos;
    if (n > avail)
        n = avail;
    memcpy(buf, img->data + img->pos, n);
    img->pos += n;
    return n;
}

static bool image_eof(const c2000_image *img) { return img->pos >= img->len; }

/* ------------------------------------------------------------------------ */
/* autobaud — OEM 0x107e60                                                   */
/* ------------------------------------------------------------------------ */
int c2000_autobaud(c2000_flasher *f)
{
    char resp = '\0';
    int tries;
    for (tries = 50; tries > 0; tries--) {        /* 0x32 */
        sleep_ns(100000000L);                     /* 100 ms */
        serial_flush(f->serial);                  /* vtable +0x70 */
        serial_write_byte(f->serial, 'A');        /* 0x41 */
        serial_read_byte(f->serial, (uint8_t *)&resp, 100);
        if (resp == 'A')                          /* echo locked */
            return 0;
    }
    return fail("baud rate detection failed");
}

/* ------------------------------------------------------------------------ */
/* send_kernel — OEM 0x107850                                                */
/* Byte-by-byte echo upload: send each byte, read it echoed back, verify.     */
/* ------------------------------------------------------------------------ */
int c2000_send_kernel(c2000_flasher *f, c2000_image *kernel,
                      c2000_progress_cb cb, void *ctx)
{
    long sent = 0;
    while (!image_eof(kernel)) {
        uint8_t b, echo;
        if (image_read(kernel, &b, 1) != 1)
            break;
        serial_write_byte(f->serial, b);
        if (cb)
            cb(ctx, (size_t)(++sent), kernel->len);
        if (image_eof(kernel))                    /* last byte: no echo wait */
            break;
        if (serial_read_byte(f->serial, &echo, 100) != 0 || echo != b)
            return fail("failed");
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* flash_app — OEM 0x107ae0                                                  */
/* The running flash kernel programs the application via a DFU block protocol. */
/* ------------------------------------------------------------------------ */

/* the 10-byte DFU start packet (DAT_0011c330): words 1be4 0000 0100 0001 e41b */
static const uint8_t k_dfu_start[10] = {
    0xe4, 0x1b, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x1b, 0xe4
};

int c2000_flash_app(c2000_flasher *f, c2000_image *app)
{
    uint16_t block_words = 0x000b;     /* first block is 11 words (22 bytes) */
    uint8_t  hdr[6];
    uint16_t csum = 0;
    uint8_t  b;
    uint16_t status, len, completion;

    /* send the DFU start packet; expect the '-' (0x2d) ack. */
    serial_write(f->serial, k_dfu_start, sizeof k_dfu_start);
    if (serial_read_byte(f->serial, &b, 100) != 0 || b != 0x2d)
        return fail("Failed sending DFU packet");

    /* stream blocks until a zero-word-count header terminates. */
    do {
        if (image_eof(app))
            break;

        if (block_words != 0) {
            uint16_t i = 0;
            for (;;) {
                image_read(app, &b, 1);
                serial_write_byte(f->serial, b);
                i++;
                csum = (uint16_t)((csum + b) & 0xffff);
                if ((uint32_t)block_words * 2 <= i)
                    break;
                if (i != 0 && (i & 0xff) == 0) {        /* every 256 bytes */
                    serial_flush(f->serial);
                    if (csum != serial_read_word(f, 100))
                        return fail("Invalid checksum");
                }
            }
        }

        /* end-of-block checksum verify */
        serial_flush(f->serial);
        if (csum != serial_read_word(f, 100))
            return fail("Invalid checksum");

        /* next-block header: 2-byte word count + 4-byte address; echo it. */
        image_read(app, hdr, 6);
        serial_write(f->serial, hdr, 6);
        block_words = (uint16_t)(hdr[0] | (hdr[1] << 8));
        csum = (uint16_t)((csum + hdr[0] + hdr[1] + hdr[2] + hdr[3]
                                + hdr[4] + hdr[5]) & 0xffff);
    } while (block_words != 0);

    /* final handshake: start header, status, completion. */
    status = serial_read_word(f, 1000);
    if (status != 0x1be4)
        return fail("Invalid start header");

    len    = serial_read_word(f, 100);     /* data length count */
    (void)serial_read_word(f, 100);        /* skip */
    status = serial_read_word(f, 100);     /* status word */
    if (len > 3) {
        uint16_t i;
        for (i = 0; (int)i < (int)(len - 2) / 2; i++)
            (void)serial_read_word(f, 100);
    }
    (void)serial_read_word(f, 100);        /* skip */
    completion = serial_read_word(f, 100);
    if (completion != 0xe41b)              /* -0x1be5 */
        return fail("Invalid start header");

    serial_write_byte(f->serial, 0x2d);    /* final ack */
    if (status == 0x1000)                  /* programmed OK */
        return 0;
    return fail("Flashing failed");
}
