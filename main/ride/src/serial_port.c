/*
 * serial_port.c — reconstructed VanMoof S5 i.MX8 `ride` service.
 *
 * common::SerialPort + common::SerialTransport: the POSIX termios serial port
 * the SSP protocol layer talks to (/dev/ttymxc3). Exclusive open (TIOCEXCL),
 * raw-mode termios, select()-with-timeout cancellable read, one-byte write.
 *
 * Program "ride" (AArch64, image base 0x100000). OEM addresses quoted per fn.
 * Sources: devices/main/common/src/serial_port.cpp
 *
 * STL string/exception glue (std::runtime_error throws, basic_string SSO) is
 * modelled at the call site — the framing/ioctl/termios logic is the real code.
 */
#include "ride_common.h"
#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>

#define SERIAL_CPP "devices/main/common/src/serial_port.cpp"

/* ioctl request numbers used verbatim from the OEM image */
#ifndef TIOCEXCL
#define TIOCEXCL 0x80045440u   /* request exclusive use of the tty */
#endif
#ifndef TCFLSH
#define TCFLSH   0x540cu       /* flush queued data (tcflush) */
#endif

/* Model of the framework throw of std::runtime_error(msg). Vendor glue: the
 * real code allocates a runtime_error (op_new 0x10) and unwinds. */
extern void serial_throw_runtime_error(const char *msg);
extern void serial_throw_runtime_error_dev(const char *prefix, const char *dev);

/*
 * serial_port_configure — OEM 0x12ce60
 *
 * tcgetattr; raw-mode mask of c_iflag/c_lflag/c_oflag, set c_cflag, VMIN=1
 * (the CONCAT26(0x100,..) writes c_cc[VTIME]/[VMIN] region), apply input and
 * output baud, tcsetattr(TCSANOW), then tcflush(TCIFLUSH).
 */
void serial_port_configure(serial_port *sp, unsigned int baud)
{
    struct termios tio;
    struct serial_port_layout *s = (struct serial_port_layout *)sp;

    memset(&tio, 0, sizeof(tio));

    if (tcgetattr(s->fd, &tio) < 0) {
        common_logf(SERIAL_CPP, 0xbb, LOG_INFO,
                    "error while getting current configuration, skip configuring serialport");
    } else {
        /* raw mode (OEM masks): c_iflag &= 0xfffffa84; c_oflag = 0;
         * c_cflag = (c_cflag & 0xfffffecf) | 0x30 (CS8|CREAD|CLOCAL);
         * c_lflag &= 0xffff7fb4 (clear ICANON/ECHO family). */
        tio.c_iflag &= 0xfffffa84u;
        tio.c_oflag  = 0;
        tio.c_cflag  = (tio.c_cflag & 0xfffffecfu) | 0x30u;
        tio.c_lflag &= 0xffff7fb4u;
        tio.c_cc[VMIN]  = 1;   /* CONCAT26(0x100,...) => VMIN=1, VTIME=0 */
        tio.c_cc[VTIME] = 0;

        if (cfsetispeed(&tio, baud) < 0 || cfsetospeed(&tio, baud) < 0) {
            common_logf(SERIAL_CPP, 0xf2, LOG_DEBUG, "error");
        }
        tcsetattr(s->fd, TCSANOW, &tio);
    }
    tcflush(s->fd, TCIFLUSH);
}

/*
 * serial_port_open — OEM 0x12cf90
 *
 * Exclusive device open: refuse if already open; require a device name; open
 * O_RDWR; ioctl TIOCEXCL; flush; configure; mark opened. Throws on every
 * failure path with a descriptive std::runtime_error.
 */
void serial_port_open(serial_port *sp)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;
    int excl = 0;

    if (s->opened) {
        common_logf(SERIAL_CPP, 0x1c, LOG_DEBUG, "SerialPort is already opened");
        return;
    }
    if (s->name == NULL || s->name[0] == '\0') {
        serial_throw_runtime_error("Error, invalid device");
        return;
    }

    s->fd = open(s->name, O_RDWR);   /* flags = 2 (O_RDWR) */
    if (s->fd == -1) {
        serial_throw_runtime_error_dev("Error, unable to open device '", s->name);
        return;
    }

    /* request exclusive access (single ioctl); if already locked, bail.
     * OEM error string for this path is "Error, device is locked '". */
    if (ioctl(s->fd, TIOCEXCL, &excl) == 0 && excl != 0) {
        close(s->fd);
        serial_throw_runtime_error_dev("Error, device is locked '", s->name);
        return;
    }

    if (ioctl(s->fd, TCFLSH) < 0) {
        close(s->fd);
        serial_throw_runtime_error_dev("Error, unable to open device exclusively '", s->name);
        return;
    }

    serial_port_configure(sp, s->baud);
    s->opened = 1;
}

/*
 * serial_port_request_stop — OEM 0x12ce50
 *
 * Raises the read-cancel flag (+0x31) so an in-flight serial_port_read aborts.
 * Used by ssp_protocol_stop during teardown.
 */
void serial_port_request_stop(serial_port *sp)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;
    s->cancel = 1;
}

/*
 * serial_port_read_impl — OEM 0x12d4e0
 *
 * select()-with-timeout read loop. Builds an fd_set over the device fd, splits
 * the microsecond timeout into {tv_sec, tv_usec}, and on readable fds reads a
 * single byte and push_back()s it into the caller's std::vector<uint8_t>.
 * Stops after `max` bytes or when the cancel flag (+0x31) is set.
 * Returns the number of bytes read.
 */
long serial_port_read(serial_port *sp, ssp_buf *out, long max, int timeout_us)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;
    long count = 0;
    unsigned char b;
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(s->fd, &rfds);

    for (;;) {
        tv.tv_sec  = timeout_us / 1000;
        tv.tv_usec = (timeout_us * 1000) % 1000000;

        if (select(s->fd + 1, &rfds, NULL, NULL, &tv) != 1)
            break;

        if (read(s->fd, &b, 1) == 1) {
            ssp_buf_push(out, b);
            count++;
        }
        if (count >= max || s->cancel != 0)
            break;

        FD_ZERO(&rfds);
        FD_SET(s->fd, &rfds);
    }
    return count;
}

/*
 * serial_port_close — OEM 0x12d2f0
 *
 * If open and fd valid, close() it; on failure throw a runtime_error. Resets
 * fd to -1 and clears the opened flag.
 */
void serial_port_close(serial_port *sp)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;

    if (!s->opened)
        return;

    if (s->fd != -1) {
        if (close(s->fd) != 0) {
            serial_throw_runtime_error_dev("Error, unable to close device '", s->name);
            return;
        }
        s->fd = -1;
    }
    s->opened = 0;
}

/*
 * serial_port_dtor — OEM 0x12d450
 *
 * SerialPort dtor: restore vtable, close the port, free the 255-byte RX buffer
 * and the (heap-allocated) device-name string.
 */
void serial_port_dtor(serial_port *sp)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;

    s->vtable = (void *)&serial_port_vtable;
    serial_port_close(sp);

    if (s->rx_begin != NULL)
        op_delete(s->rx_begin, 0xff);          /* 255-byte RX buffer */
    if (s->name != s->name_sso)
        op_delete(s->name, 0);                 /* heap string body */
}

/*
 * serial_transport_ctor — OEM 0x12cd80
 *
 * SerialTransport/SerialPort ctor. Moves the std::string device name into the
 * SSO/heap slot, stores the baud, clears fd/flags, and allocates the 255-byte
 * RX scratch buffer (begin/end/cap pointers).
 */
void serial_transport_ctor(serial_port *sp, ride_string *name, unsigned int baud)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;
    char *src = name->ptr;

    s->vtable   = (void *)&serial_port_vtable;
    s->name     = s->name_sso;

    if (src == name->sso) {
        /* short string: copy the two SSO qwords inline */
        s->name_sso_q[0] = name->sso_q[0];
        s->name_sso_q[1] = name->sso_q[1];
    } else {
        /* long string: steal the heap pointer + capacity */
        s->name        = src;
        s->name_sso_q[0] = name->sso_q[0];
    }
    s->name_len = name->len;

    /* leave the moved-from std::string empty/SSO */
    name->ptr    = name->sso;
    name->len    = 0;
    name->sso[0] = 0;

    s->baud   = baud;
    s->fd     = 0;
    s->opened = 0;
    s->cancel = 0;

    s->rx_begin = NULL;
    s->rx_end   = NULL;
    s->rx_cap   = NULL;

    s->rx_begin = (uint8_t *)op_new(0xff);     /* 255-byte RX buffer */
    s->rx_cap   = s->rx_begin + 0xff;
    memset(s->rx_begin, 0, 0xff);
    s->rx_end   = s->rx_begin + 0xff;
}

/*
 * serial_transport_write_byte — OEM 0x12c6d0
 *
 * Writes one byte to the fd; throws std::runtime_error on write()==-1.
 */
void serial_transport_write_byte(serial_port *sp, uint8_t byte)
{
    struct serial_port_layout *s = (struct serial_port_layout *)sp;
    uint8_t b = byte;

    if (write(s->fd, &b, 1) != -1)
        return;

    serial_throw_runtime_error("failed to write to serial port");
}

/*
 * serial_transport_read_byte — OEM 0x12d6d0
 *
 * Reads a single byte through serial_port_read with the given timeout (the SSP
 * RX loop passes 10ms). Returns 0 and stores the byte on success, -1 on
 * timeout (no bytes read).
 */
int serial_transport_read_byte(serial_port *sp, uint8_t *out, int timeout_us)
{
    ssp_buf tmp;
    int rc;

    ssp_buf_init(&tmp);

    if (serial_port_read(sp, &tmp, 1, timeout_us) == 0) {
        rc = -1;
        if (tmp.data == NULL) {
            /* nothing allocated: nothing to free */
            return rc;
        }
    } else {
        rc = 0;
        *out = tmp.data[0];
    }
    ssp_buf_free(&tmp);
    return rc;
}
