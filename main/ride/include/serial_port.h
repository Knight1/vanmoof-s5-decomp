/*
 * serial_port.h — module-private model for the reconstructed `ride`
 * common::SerialPort / common::SerialTransport (OEM serial_port.cpp).
 *
 * Included AFTER ride_common.h. Mirrors the observed object layout from the
 * OEM ctor (serial_transport_ctor 0x12cd80): an 8-qword object whose tail
 * holds the device-name string slot, the termios fd/flags, and a 255-byte RX
 * scratch buffer.
 */
#ifndef RIDE_SERIAL_PORT_H
#define RIDE_SERIAL_PORT_H

#include "ride_common.h"

/* libstdc++ std::string (libc++abi SSO) model, used by the move ctor. */
typedef struct ride_string {
    char     *ptr;        /* +0x00 data pointer (== sso when short)   */
    uint64_t  len;        /* +0x08 length                            */
    union {
        char     sso[16]; /* +0x10 in-place small-string buffer       */
        uint64_t sso_q[2];
    };
} ride_string;

/*
 * common::SerialPort concrete layout (offsets verbatim from the decomp):
 *   +0x00 vtable
 *   +0x08 name           (char*, == name_sso when short)
 *   +0x10 name_len
 *   +0x18 name_sso[16]   (inline small-string buffer)
 *   +0x28 baud           (uint)
 *   +0x2c fd             (int, -1 when closed)
 *   +0x30 opened         (byte)
 *   +0x31 cancel         (byte, read-cancel flag)
 *   +0x38 rx_begin       (255-byte RX scratch buffer begin)
 *   +0x40 rx_end
 *   +0x48 rx_cap
 */
struct serial_port_layout {
    void     *vtable;       /* +0x00 */
    char     *name;         /* +0x08 */
    uint64_t  name_len;     /* +0x10 */
    union {
        char     name_sso[16];  /* +0x18 */
        uint64_t name_sso_q[2];
    };
    unsigned int baud;      /* +0x28 */
    int       fd;           /* +0x2c */
    uint8_t   opened;       /* +0x30 */
    uint8_t   cancel;       /* +0x31 */
    uint8_t   _pad[6];
    uint8_t  *rx_begin;     /* +0x38 */
    uint8_t  *rx_end;       /* +0x40 */
    uint8_t  *rx_cap;       /* +0x48 */
};

/* SerialPort vtable symbol (OEM &DAT_0016f440), supplied by the framework. */
extern const void *const serial_port_vtable;

/* Public API (OEM names + ABI). */
void serial_port_open(serial_port *sp);                                  /* 0x12cf90 */
void serial_port_configure(serial_port *sp, unsigned int baud);          /* 0x12ce60 */
long serial_port_read(serial_port *sp, ssp_buf *out, long max, int timeout_us); /* 0x12d4e0 */
void serial_port_close(serial_port *sp);                                 /* 0x12d2f0 */
void serial_port_request_stop(serial_port *sp);                          /* 0x12ce50 */
void serial_port_dtor(serial_port *sp);                                  /* 0x12d450 */
void serial_transport_ctor(serial_port *sp, ride_string *name, unsigned int baud); /* 0x12cd80 */
void serial_transport_write_byte(serial_port *sp, uint8_t byte);         /* 0x12c6d0 */
int  serial_transport_read_byte(serial_port *sp, uint8_t *out, int timeout_us);    /* 0x12d6d0 */

#endif /* RIDE_SERIAL_PORT_H */
