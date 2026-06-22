/*
 * power_button.c — PowerButtonService logic for the VanMoof S5 i.MX8 `ux`
 * service. Behaviour reconstruction of devices/main/ux/src/power_button.cpp.
 * Program "ux" (AArch64, image base 0x100000). OEM addresses in comments.
 *
 * The std::function listener fan-out (under the instance mutex) and the
 * std::thread / std::string plumbing are VENDOR glue, modelled at the call
 * site and not reconstructed.
 */
#include "ux_common.h"
#include "power_button.h"

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

struct power_button_service {
    void *vtable;           /* +0x00  PTR_power_button_dtor_001fa608 */
    char  _pad0[0x120 - 0x08];
    int   prev_value;       /* +0x120 */
    int   press_count;      /* +0x124 */
    char  _pad1[0x130 - 0x128];
    char  running;          /* +0x130 */
    char  _pad2[0x134 - 0x131];
    int   fd;               /* +0x134 input-event device fd */
    char  open_done;        /* +0x138 */
};

/* EVIOCGBIT(0, ...) packs the supported event-type bitmask; bit 1 == EV_KEY. */
#ifndef EVIOCGBIT0
#define EVIOCGBIT0 0x80084520u
#endif
#define KEY_POWER  0x74
#define EV_KEY     0x01

/* ---- power_button_open_device @0x177c10 ----------------------------------
 * Opens the power-button input device. Probes the PRE-PVT path first; if that
 * board marker opens, logs "SOC board is a PRE-PVT" and uses its event node,
 * otherwise opens the caller-supplied path. Logs an error and zeroes the fd on
 * failure. (Paths come from the OEM .rodata string table.)
 */
void power_button_open_device(int *fd_out, void **dev_path)
{
    int probe;
    unsigned fd;

    fd_out[0] = 0;
    ((char *)fd_out)[4] = 0;   /* open marker low byte */

    probe = open(/* PRE-PVT board marker node */ "", O_RDONLY | O_NONBLOCK);
    if (probe < 0) {
        fd = (unsigned)open((const char *)*dev_path, O_RDONLY | O_NONBLOCK);
        *fd_out = (int)fd;
    } else {
        common_logf("devices/main/ux/src/power_button.cpp", 0x1e, LOG_WARN,
                    "SOC board is a PRE-PVT");
        close(probe);
        fd = (unsigned)open(/* PRE-PVT event node */ "", O_RDONLY | O_NONBLOCK);
        *fd_out = (int)fd;
    }

    if ((int)fd < 0) {
        common_logf("devices/main/ux/src/power_button.cpp", 0x29, LOG_DEBUG,
                    "Error, unable to open device: %s %d", (const char *)*dev_path, fd);
        *fd_out = 0;
    }
    ((int *)fd_out)[1] = 1;   /* open_done = true */
}

/* ---- power_button_supports_key_events @0x177b30 --------------------------
 * Returns whether the opened device reports EV_KEY support via EVIOCGBIT(0).
 * variant==1 short-circuits to "yes"; an unopened fd yields "no".
 */
unsigned power_button_supports_key_events(int *fd, int variant)
{
    unsigned result = 1;

    if (variant != 1) {
        result = 0;
        if (*fd != 0) {
            unsigned long evbits = 0;
            ioctl(*fd, EVIOCGBIT0, &evbits);
            result = (unsigned)(evbits >> 1) & 1u;   /* bit 1 == EV_KEY */
        }
    }
    return result;
}

/* ---- power_button_poll_read_event @0x177990 ------------------------------
 * Polls the device fd (1s timeout). On a readable revent reads a full 24-byte
 * input_event and marks it valid; poll errors log "cannot poll device".
 */
void power_button_poll_read_event(power_input_event *out, int *pollfd)
{
    int rc = poll((struct pollfd *)pollfd, 1, 1000);

    if (rc < 0) {
        common_logf("devices/main/ux/src/power_button.cpp", 0x4e, LOG_INFO,
                    "Error, cannot poll device. %d", (unsigned)errno);
    } else if (rc != 0 && (((struct pollfd *)pollfd)->revents & POLLIN) != 0) {
        uint64_t buf[3] = { 0, 0, 0 };
        ssize_t n = read(*pollfd, buf, 0x18);
        if (n == 0x18) {
            out->tv_sec  = buf[0];
            out->tv_usec = buf[1];
            *(uint64_t *)&out->type = buf[2];
            out->valid = 1;
            return;
        }
    }
    out->valid = 0;
}

/* ---- power_button_ctor @0x178090 -----------------------------------------
 * Builds the PowerButtonService vtable, arms the 1000ms hold timer, opens the
 * input device, warns if it lacks key-event support, and spawns the reader
 * thread (power_button_reader_loop).
 */
void power_button_ctor(power_button_service *self, void **dev_path, int variant)
{
    char supported;

    self->prev_value  = 0;
    self->press_count = 0;
    self->running     = 1;   /* *(undefined1*)(param_1+0x26) = 1 -> running flag */

    power_button_open_device(&self->fd, dev_path);
    supported = (char)power_button_supports_key_events(&self->fd, variant);
    if (supported == 0) {
        common_logf("devices/main/ux/src/power_button.cpp", 0x72, LOG_DEBUG,
                    "Error, file does not support key events: %s", (const char *)*dev_path);
    }

    /* spawn reader pthread with run==power_button_reader_loop. */
    {
        void *thread_ctx = self;
        (void)thread_ctx;   /* pthread_create modelled (FUN_0010df30). */
    }
}

/* ---- power_button_dtor @0x1782d0 -----------------------------------------
 * Stops the reader thread (clears running, joins), closes the device fd, and
 * tears the base service down.
 */
void power_button_dtor(power_button_service *self)
{
    self->running = 0;
    /* signal + join the reader thread (FUN_0010d8d0 on +0x25). */
    if (self->fd > 0)
        close(self->fd);
}

/* ---- power_button_reader_loop @0x178370 ----------------------------------
 * Reader thread body. While running and the fd is valid, polls/reads input
 * events, filtering EV_KEY/KEY_POWER. On press (value==1, prev==0): fires the
 * button-down listeners, bumps the multi-press counter if the hold timer is
 * still active, and arms a 1000ms timer. On release (value==0, prev==1): arms
 * a 400ms multi-press window (unless the counter saturated at 0xff, which
 * resets it) and fires the button-up listeners. Tracks prev value at +0x120.
 */
void power_button_reader_loop(power_button_service **arg)
{
    power_button_service *self = *arg;

    while (self->running != 0) {
        power_input_event ev;
        int value;

        self = *arg;
        if (self->fd == 0)
            continue;

        power_button_poll_read_event(&ev, &self->fd);
        value = ev.value;
        if (ev.valid == 0)
            continue;

        self = *arg;
        if (ev.type != EV_KEY || ev.code != KEY_POWER)
            continue;

        if (self->prev_value == 0 && value == 1) {
            /* button down */
            power_button_listeners_fire_down(self);
            if (power_button_timer_active((char *)self + 0x70) != 0)
                self->press_count++;
            power_button_timer_arm((char *)self + 0x70, 1000);
            self = *arg;
            self->prev_value = value;
        } else if (value == 0 && self->prev_value == 1) {
            /* button up */
            if (self->press_count == 0xff) {
                self->press_count = 0;
            } else {
                power_button_timer_arm((char *)self + 0x70, 400);
                self = *arg;
            }
            power_button_listeners_fire_up(self);
            self = *arg;
            self->prev_value = value;
        } else {
            self->prev_value = value;
        }
        self = *arg;
    }

    /* on exit, close the fd. */
    self = *arg;
    if (self->fd > 0) {
        close(self->fd);
        self->fd = -1;
    }
}
