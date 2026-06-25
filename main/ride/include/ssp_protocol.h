/*
 * ssp_protocol.h — module-private model for the reconstructed VanMoof S5
 * i.MX8 `ride` service SSP serial protocol (program "ride", base 0x100000).
 *
 * SspProtocol speaks a SLIP-framed, CRC-16/MODBUS-checked binary protocol to
 * the motor controller over /dev/ttymxc3. A frame on the wire is:
 *
 *     0xC0  [0x14, type, seq, opcode_lo, opcode_hi, payload…]  CRC_lo CRC_hi  0xC0
 *      \___ SLIP start                                       \___ MODBUS —/  \_ SLIP end
 *
 * Within the frame body, the two SLIP control octets are byte-stuffed:
 *     0xC0 -> 0xDB 0xDC     (END  escape)
 *     0xDB -> 0xDB 0xDD     (ESC  escape)
 *
 * frame type values: 0x05 = ACK, 0x06 = read/get (expects ack), 0x07 = notify.
 *
 * Included AFTER ride_common.h. Only the SSP-specific glue lives here; the
 * SspProtocol object layout (an opaque `ssp_proto`) and the std::deque / thread
 * machinery are vendor and modelled as helpers, not reconstructed.
 */
#ifndef SSP_PROTOCOL_H
#define SSP_PROTOCOL_H

#include "ride_common.h"

/* SLIP control octets (signed-char compares in the OEM: 0xC0 == -0x40,
 * 0xDB == -0x25, 0xDC == -0x24, 0xDD == -0x23). */
#define SSP_SLIP_END      0xC0u   /* frame delimiter                       */
#define SSP_SLIP_ESC      0xDBu   /* escape prefix                         */
#define SSP_SLIP_ESC_END  0xDCu   /* escaped 0xC0                          */
#define SSP_SLIP_ESC_ESC  0xDDu   /* escaped 0xDB                          */

/* Frame header layout. */
#define SSP_FRAME_MAGIC   0x14u   /* byte[0] of every frame                */
#define SSP_TYPE_ACK      0x05u   /* ack                                   */
#define SSP_TYPE_GET      0x06u   /* read / get (acked, dispatched to subs) */
#define SSP_TYPE_NOTIFY   0x07u   /* unsolicited notify                    */

/* CRC-16/MODBUS: reflected poly 0xA001, seed 0xFFFF, LSB-first. */
#define SSP_CRC16_POLY    0xA001u
#define SSP_CRC16_SEED    0xFFFFu

/* RX byte read timeout (ms) passed to serial_transport_read_byte(). */
#define SSP_RX_TIMEOUT_MS 10

/* ---- SspProtocol object (opaque ssp_proto, accessed via these accessors) --
 * The OEM object is ~0x1c0 bytes. We model only the fields the SSP code
 * touches, behind getters/setters so the .c keeps the OEM field offsets in
 * comments without exposing a by-value struct. */
struct ssp_proto; /* defined opaque in ride_common.h */

/* serial transport (common::SerialPort over termios). */
serial_port *ssp_serial(ssp_proto *p);            /* +0x68 */
int   serial_transport_read_byte(serial_port *s, char *out, int timeout_ms);
void  serial_transport_write_byte(serial_port *s, int byte);
void  serial_port_open(serial_port *s);
void  serial_port_close(serial_port *s);
void  serial_port_request_stop(serial_port *s);

bool  ssp_stop_requested(ssp_proto *p);           /* +0x70 (uint32 flag)   */
void  ssp_set_stop(ssp_proto *p, bool v);

/* TX-frame std::deque<std::vector<uint8_t>> at +0xf8 (front=0x108 back=0x128),
 * protected by mutex +0x178 with cv +0x148. RX subscriber list at +0x35.0x36.
 * Modelled as opaque queue/list helpers — vendor STL glue at the call site. */
void  ssp_txq_lock(ssp_proto *p);                 /* mutex +0x178           */
void  ssp_txq_unlock(ssp_proto *p);
void  ssp_txq_wait(ssp_proto *p);                 /* cv wait while empty    */
void  ssp_txq_notify(ssp_proto *p);               /* cv +0x148 notify       */
bool  ssp_txq_empty(ssp_proto *p);
void  ssp_txq_push(ssp_proto *p, const ssp_buf *frame);   /* emplace_back   */
bool  ssp_txq_pop(ssp_proto *p, ssp_buf *out);            /* pop_front      */

/* RX dispatch into the strategy/subscriber objects. The OEM calls through the
 * vtables of objects held in the +0x35..+0x36 vector and through the
 * RideService vtable (this+0x10 -> ssp_enqueue_frame, this+0x18 -> notify). */
int   ssp_sub_count(ssp_proto *p);
void *ssp_sub_at(ssp_proto *p, int i);
/* vtable+0x10: bool handle_get(sub, opcode, ssp_buf *reply) */
bool  ssp_sub_handle_get(void *sub, uint8_t opcode, ssp_buf *reply);
/* vtable+0x18: void handle_notify(sub, opcode, uint16_t len, const uint8_t *payload) */
void  ssp_sub_handle_notify(void *sub, uint8_t opcode, uint16_t len, const uint8_t *payload);
void  ssp_self_enqueue(ssp_proto *p, ssp_buf *frame);     /* this+0x10      */
void  ssp_self_notify(ssp_proto *p, void *field);         /* this+0x18      */

/* per-counter event taps (the ExclusiveMonitor inc loops in the OEM are atomic
 * statistics counters; modelled as a single helper). */
void  ssp_stat_inc(ssp_proto *p, int which);

/* worker-thread spawn/join glue (std::thread fields +0x08 rx, +0x10 tx,
 * +0x18 dispatch). */
void *ssp_thread_field(ssp_proto *p, int which);
void  ssp_thread_set(ssp_proto *p, int which, void *t);
void  ssp_thread_join_field(ssp_proto *p, int which);

/* module entry points (OEM names). */
void ssp_crc16_into(const uint8_t *data, int len, uint16_t *crc); /* 0x12a340 */
void ssp_frame_begin(ssp_buf *f, uint16_t opcode, char notify);  /* 0x1224c0 */
void ssp_frame_append(ssp_buf *f, uint16_t v);                   /* 0x122460 */
void ssp_frame_append_u16(ssp_buf *f, uint16_t v);              /* 0x1223e0 */
void ssp_write_escaped_byte(ssp_proto *p, uint8_t b);            /* 0x124090 */
void ssp_enqueue_frame(ssp_proto *p, ssp_buf *frame);           /* 0x122bf0 */
void ssp_send_ack(ssp_proto *p, uint8_t ack_seq);               /* 0x122270 */
void ssp_receive_loop(ssp_proto *p);                            /* 0x122d50 */
void ssp_send_loop(ssp_proto *p);                               /* 0x124100 */
ssp_proto *ssp_protocol_ctor(ssp_proto *p, serial_port *s);     /* 0x122630 */
void ssp_protocol_start(ssp_proto *p);                          /* 0x1245a0 */
void ssp_protocol_start_dispatch_thread(ssp_proto *p);          /* 0x121f70 */
void ssp_protocol_stop(ssp_proto *p);                           /* 0x124700 */
void ssp_protocol_dtor(ssp_proto *p);                           /* 0x124780 */
void ssp_protocol_delete(ssp_proto *p);                         /* 0x124930 */

/* modelled SspProtocol object + queue lifecycle helpers (vendor STL glue). */
void ssp_proto_zero_init(ssp_proto *p);
void ssp_proto_set_serial(ssp_proto *p, serial_port *s);
void ssp_proto_reset_vtable(ssp_proto *p);
void ssp_txq_init(ssp_proto *p);
void ssp_txq_destroy(ssp_proto *p);
void ssp_cv_destroy(ssp_proto *p);
void ssp_subs_free(ssp_proto *p);
void ssp_payload_read_u8(const void *payload, void *out);

#endif /* SSP_PROTOCOL_H */
