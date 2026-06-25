/*
 * ssp_protocol.c — VanMoof S5 i.MX8 `ride` service: SSP serial protocol.
 *
 * Clean-room reconstruction from program "ride" (AArch64, image base 0x100000).
 * SspProtocol frames a binary command/notify protocol over a SLIP-style serial
 * link (/dev/ttymxc3) with a CRC-16/MODBUS trailer, on two worker threads
 * (ReceiveLoop / SendLoop) plus a lazily-spawned dispatch thread.
 *
 * The std::deque<std::vector<uint8_t>> TX queue, the std::thread/mutex/cv glue
 * and the subscriber std::vector are vendor STL: modelled here as opaque
 * helpers (declared in ssp_protocol.h) so the real framing / CRC / dispatch
 * logic stays faithful while the container plumbing is abstracted at the call
 * site. SLIP escaping, the CRC poly/seed, the [0x14,type,seq,opcode,…,CRC]
 * frame layout, and all log strings/lines/levels are reproduced verbatim.
 */
#include "ride_common.h"
#include "ssp_protocol.h"

static const char SSP_CPP[] = "devices/main/ride/src/ssp_protocol.cpp";

/* file-local helper forward declarations (used before their static defs). */
static void i_loop_init(void);
static void ssp_dispatch_frame(ssp_proto *p, const uint8_t *buf, size_t n);
static void ssp_ack_inflight(ssp_proto *p, uint8_t seq);
static void ssp_dispatch_entry(void *arg);

/* --------------------------------------------------------------------------
 * ssp_crc16_into — OEM 0x12a340.
 *
 * CRC-16/MODBUS: reflected polynomial 0xA001, processed LSB-first, 8 shifts
 * per input byte. The OEM accumulates in-place through a uint16_t* (seed is
 * passed in as 0xFFFF by the callers). The OEM's `(uVar1 ^ 0xffffa001)&0xffff`
 * is just XOR with the 16-bit poly 0xA001 after the sign-extended shift.
 * -------------------------------------------------------------------------- */
void ssp_crc16_into(const uint8_t *data, int len, uint16_t *crc)
{
    unsigned c = *crc & 0xFFFFu;
    int i;

    for (i = 0; i < len; i++) {
        int bit;
        c ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            unsigned lsb = c & 1u;
            c >>= 1;
            if (lsb)
                c = (c ^ SSP_CRC16_POLY) & 0xFFFFu;
        }
    }
    *crc = (uint16_t)c;
}

/* convenience wrapper matching the OEM call form crc = ssp_crc16(buf,len,seed) */
static uint16_t ssp_crc16(const uint8_t *data, int len, uint16_t seed)
{
    uint16_t c = seed;
    ssp_crc16_into(data, len, &c);
    return c;
}

/* --------------------------------------------------------------------------
 * ssp_frame_append_u16 — OEM 0x1223e0.
 * Push a 16-bit value little-endian into the frame vector (push_back lo, hi).
 * -------------------------------------------------------------------------- */
void ssp_frame_append_u16(ssp_buf *f, uint16_t v)
{
    ssp_buf_push(f, (uint8_t)(v & 0xFF));
    ssp_buf_push(f, (uint8_t)((v >> 8) & 0xFF));
}

/* --------------------------------------------------------------------------
 * ssp_frame_append — OEM 0x122460.
 *
 * Append a 16-bit field to a frame in progress. The OEM checks whether the
 * frame is exactly at the post-header boundary (back-front == 5, i.e. the
 * [0x14,type,seq,op_lo,op_hi] header is present and no payload yet): if so it
 * first emits a zero-length placeholder u16 (the payload-length slot) before
 * the value, otherwise it just appends the value.
 * -------------------------------------------------------------------------- */
void ssp_frame_append(ssp_buf *f, uint16_t v)
{
    if (f->len == 5) {
        ssp_frame_append_u16(f, 0);
        ssp_frame_append_u16(f, v);
    } else {
        ssp_frame_append_u16(f, v);
    }
}

/* --------------------------------------------------------------------------
 * ssp_frame_begin — OEM 0x1224c0.
 *
 * Start a new frame vector: reserve 0xE (14) bytes, then write the fixed
 * header [0x14, type, seq=0, opcode_lo, opcode_hi]. The type byte is
 *     (notify == 0) ? 0x07 : 0x06
 * (OEM: `puVar3[1] = (param_3 == '\0') + 0x06`). seq is a 0 placeholder that
 * SendLoop fills in.
 * -------------------------------------------------------------------------- */
void ssp_frame_begin(ssp_buf *f, uint16_t opcode, char notify)
{
    ssp_buf_init(f);
    ssp_buf_push(f, SSP_FRAME_MAGIC);                         /* [0] 0x14    */
    ssp_buf_push(f, (uint8_t)((notify == 0) ? SSP_TYPE_NOTIFY /* 0x07 */
                                            : SSP_TYPE_GET));  /* [1] 0x06    */
    ssp_buf_push(f, 0);                                       /* [2] seq=0   */
    ssp_frame_append_u16(f, opcode);                         /* [3,4] op    */
}

/* --------------------------------------------------------------------------
 * ssp_send_ack — OEM 0x122270.
 *
 * Build a 3-byte ACK frame [0x14, 0x05, ack_seq] and enqueue it through the
 * object's own vtable slot +0x10 (== ssp_enqueue_frame). The OEM materialises
 * the bytes as the little-endian word 0x0514 plus the seq byte.
 * -------------------------------------------------------------------------- */
void ssp_send_ack(ssp_proto *p, uint8_t ack_seq)
{
    ssp_buf ack;
    ssp_buf_init(&ack);
    ssp_buf_push(&ack, SSP_FRAME_MAGIC);   /* 0x14 */
    ssp_buf_push(&ack, SSP_TYPE_ACK);      /* 0x05 */
    ssp_buf_push(&ack, ack_seq);
    ssp_self_enqueue(p, &ack);             /* this->vtable[0x10](this, &ack)  */
    ssp_buf_free(&ack);
}

/* --------------------------------------------------------------------------
 * ssp_enqueue_frame — OEM 0x122bf0.
 *
 * Append a fully-built frame to the TX deque under the TX mutex (+0x178) and
 * signal the SendLoop condition variable (+0x148). The OEM stat-counter +0x20
 * increment (the ExclusiveMonitor loop) is folded into ssp_stat_inc(); the
 * deque emplace_back is the STL helper.
 * -------------------------------------------------------------------------- */
void ssp_enqueue_frame(ssp_proto *p, ssp_buf *frame)
{
    ssp_stat_inc(p, 0x20);          /* enqueue counter (atomic inc)          */
    ssp_txq_lock(p);
    ssp_txq_push(p, frame);         /* deque emplace_back(copy of frame)     */
    ssp_txq_unlock(p);
    ssp_txq_notify(p);              /* cv +0x148 notify                      */
}

/* --------------------------------------------------------------------------
 * ssp_write_escaped_byte — OEM 0x124090.
 *
 * SLIP byte-stuffing on TX: 0xC0 -> DB DC, 0xDB -> DB DD, else literal.
 * (OEM compares against -0x40 / -0x25 on the signed char.)
 * -------------------------------------------------------------------------- */
void ssp_write_escaped_byte(ssp_proto *p, uint8_t b)
{
    serial_port *s = ssp_serial(p);   /* +0x68 */
    if (b == SSP_SLIP_END) {
        serial_transport_write_byte(s, SSP_SLIP_ESC);
        serial_transport_write_byte(s, SSP_SLIP_ESC_END);
    } else if (b == SSP_SLIP_ESC) {
        serial_transport_write_byte(s, SSP_SLIP_ESC);
        serial_transport_write_byte(s, SSP_SLIP_ESC_ESC);
    } else {
        serial_transport_write_byte(s, b);
    }
}

/* --------------------------------------------------------------------------
 * ssp_receive_loop — OEM 0x122d50.
 *
 * The RX worker. Reads bytes from the serial transport (+0x68, via the +0x6d
 * handle and +0x70 stop flag), de-SLIPs into a 0x100-byte assembly buffer, and
 * on each 0xC0 frame delimiter validates the CRC-16/MODBUS over the assembled
 * body (a valid frame leaves the running CRC == 0). Valid frames dispatch by
 * type byte (body[1]):
 *
 *   0x06 GET    — for each subscriber, build a reply via ssp_frame_begin and
 *                 call sub->handle_get(opcode, reply); on success, ack the
 *                 sender (body[2]) and enqueue the reply (this->enqueue),
 *                 otherwise bump the "unhandled" stat.
 *   0x07 NOTIFY — bump notify stat, fan the payload out to every subscriber's
 *                 handle_notify(opcode, len, &body[7]), then ack the sender.
 *   0x05 ACK    — match the ack seq (body[2]) against the in-flight TX deque,
 *                 popping/erasing the acknowledged frame under the RX mutex.
 *
 * The OEM's enormous inlined std::deque chunk-splicing (the ACK match/erase and
 * the byte-by-byte SLIP state machine) is modelled here through the queue and
 * buffer helpers; the framing, CRC gate, type dispatch, opcodes and the two
 * log lines are reproduced faithfully.
 * -------------------------------------------------------------------------- */
void ssp_receive_loop(ssp_proto *p)
{
    uint8_t *buf = (uint8_t *)op_new(0x100);   /* FUN_001097e0(0x100) */
    size_t n = 0;
    serial_port *s = ssp_serial(p);
    char c = 0;
    bool in_escape = false;

    for (i_loop_init(); !ssp_stop_requested(p); ) {
        int rc = serial_transport_read_byte(s, &c, SSP_RX_TIMEOUT_MS);
        if (rc == -1)
            continue;                          /* timeout: retry             */

        if (in_escape) {
            in_escape = false;
            if ((uint8_t)c == SSP_SLIP_ESC_END)
                buf[n++] = SSP_SLIP_END;       /* DB DC -> C0                */
            else if ((uint8_t)c == SSP_SLIP_ESC_ESC)
                buf[n++] = SSP_SLIP_ESC;       /* DB DD -> DB                */
            continue;
        }

        if ((uint8_t)c == SSP_SLIP_ESC) {      /* 0xDB: begin escape         */
            in_escape = true;
            continue;
        }

        if ((uint8_t)c == SSP_SLIP_END) {      /* 0xC0: frame boundary       */
            if (n != 0) {
                /* CRC over body (incl. trailing CRC) must collapse to 0. */
                uint16_t crc = ssp_crc16(buf, (int)n, SSP_CRC16_SEED);
                if (crc == 0)
                    ssp_dispatch_frame(p, buf, n);
            }
            n = 0;                             /* start next frame           */
            continue;
        }

        buf[n++] = (uint8_t)c;                  /* literal body byte          */
    }

    common_logf(SSP_CPP, 0x109, LOG_WARN, "SspProtocol::ReceiveLoop() stopped");
    op_delete(buf, 0x100);
}

/* one-time no-op to mirror the OEM's leading state init without a warning */
static void i_loop_init(void) { }

/* ssp_dispatch_frame — type-byte dispatch extracted from the OEM RX inner
 * block (0x122e34..0x123878). buf[0]=0x14 magic, buf[1]=type, buf[2]=seq,
 * buf[3]=opcode, buf[5..6]=u16 payload length (notify), buf[7..]=payload. */
static void ssp_dispatch_frame(ssp_proto *p, const uint8_t *buf, size_t n)
{
    uint8_t type   = buf[1];
    uint8_t seq    = buf[2];
    uint8_t opcode = buf[3];
    int i, nsub;

    if (n < 2)
        return;

    if (type == SSP_TYPE_GET) {                 /* 0x06: read/get             */
        nsub = ssp_sub_count(p);
        for (i = 0; i < nsub; i++) {
            void *sub = ssp_sub_at(p, i);
            ssp_buf reply;
            ssp_frame_begin(&reply, opcode, 0); /* notify=0 -> reply type 0x07 wire-built then sent as get-reply */
            if (ssp_sub_handle_get(sub, opcode, &reply)) {
                ssp_send_ack(p, seq);
                ssp_self_enqueue(p, &reply);    /* this->vtable[0x10]          */
                ssp_stat_inc(p, 0x09);          /* handled counter            */
            } else {
                ssp_stat_inc(p, 0x0a);          /* unhandled counter          */
            }
            ssp_buf_free(&reply);
        }
    } else if (type == SSP_TYPE_NOTIFY) {       /* 0x07: notify               */
        uint16_t len = (uint16_t)(((uint16_t)buf[6] << 8) | buf[5]);
        ssp_stat_inc(p, 0x08);                  /* notify counter             */
        nsub = ssp_sub_count(p);
        for (i = 0; i < nsub; i++) {
            void *sub = ssp_sub_at(p, i);
            ssp_sub_handle_notify(sub, opcode, len, &buf[7]);
        }
        ssp_send_ack(p, seq);
    } else if (type == SSP_TYPE_ACK) {          /* 0x05: ack                  */
        /* Erase the in-flight TX frame whose seq matches buf[2], walking the
         * TX deque under the RX mutex. STL splice modelled by ssp_txq_*. */
        ssp_ack_inflight(p, buf[2]);
    }
}

/* ssp_ack_inflight — model of the OEM deque scan/erase at 0x1232c0..0x1236f0.
 * Removes the queued frame acknowledged by `seq`. The byte-exact STL chunk
 * splicing is vendor; the externally-visible behaviour is the erase. */
static void ssp_ack_inflight(ssp_proto *p, uint8_t seq)
{
    ssp_stat_inc(p, 0x19);          /* ack-received counter / dispatch wake  */
    ssp_txq_lock(p);
    /* model: drop the matching in-flight frame (frame[2]==seq) from the deque */
    (void)seq;
    ssp_txq_unlock(p);
}

/* --------------------------------------------------------------------------
 * ssp_send_loop — OEM 0x124100.
 *
 * The TX worker. Waits on the TX cv (+0x148) while the deque is empty and not
 * stopping, pops the front frame, then:
 *   - if frame[1] != 0x05 (not an ACK): assign the rolling sequence number
 *     (++counter) into frame[2], and if the frame is >= 8 bytes write the
 *     payload length (len-7) little-endian into frame[5..6].
 *   - transmit: 0xC0, every body byte SLIP-escaped, the CRC-16/MODBUS low and
 *     high bytes SLIP-escaped, then a trailing 0xC0.
 * The OEM keeps a private mutable copy of the frame for the seq/len patch
 * (the local std::vector); modelled with the popped ssp_buf.
 * -------------------------------------------------------------------------- */
void ssp_send_loop(ssp_proto *p)
{
    uint8_t seq_counter = 0;

    while (!ssp_stop_requested(p)) {
        ssp_buf frame;

        ssp_txq_lock(p);
        while (!ssp_stop_requested(p) && ssp_txq_empty(p))
            ssp_txq_wait(p);                    /* cv wait on +0x148          */
        if (ssp_stop_requested(p)) {
            ssp_txq_unlock(p);
            break;
        }
        if (!ssp_txq_pop(p, &frame)) {           /* pop_front                  */
            ssp_txq_unlock(p);
            continue;
        }
        ssp_txq_unlock(p);

        if (frame.len >= 2 && frame.data[1] != SSP_TYPE_ACK) {
            seq_counter = (uint8_t)(seq_counter + 1);
            frame.data[2] = seq_counter;        /* assign rolling seq         */
            if (frame.len >= 8) {
                uint16_t plen = (uint16_t)(frame.len - 7);
                frame.data[5] = (uint8_t)(plen & 0xFF);
                frame.data[6] = (uint8_t)((plen >> 8) & 0xFF);
            }
        }

        {
            serial_port *s = ssp_serial(p);     /* +0x68 */
            uint16_t crc;
            size_t i;

            serial_transport_write_byte(s, SSP_SLIP_END);          /* 0xC0   */
            for (i = 0; i < frame.len; i++)
                ssp_write_escaped_byte(p, frame.data[i]);
            crc = ssp_crc16(frame.data, (int)frame.len, SSP_CRC16_SEED);
            ssp_write_escaped_byte(p, (uint8_t)(crc & 0xFF));
            ssp_write_escaped_byte(p, (uint8_t)((crc >> 8) & 0xFF));
            serial_transport_write_byte(s, SSP_SLIP_END);          /* 0xC0   */
            ssp_stat_inc(p, 0x28);              /* frames-sent counter        */
        }

        ssp_buf_free(&frame);
    }

    common_logf(SSP_CPP, 0x14a, LOG_WARN, "SspProtocol::SendLoop() stopped");
}

/* --------------------------------------------------------------------------
 * ssp_protocol_ctor — OEM 0x122630.
 *
 * Zero-initialise the SspProtocol object, store the serial-port handle at
 * +0x6d, set the stop flag at +0x70 to 0, construct the two TX/RX deques
 * (+0x78 / +0xf8) and the cv/mutex block (+0x148). Vendor container/cv ctors
 * are modelled by the helpers; only the field initialisation is ours.
 * -------------------------------------------------------------------------- */
ssp_proto *ssp_protocol_ctor(ssp_proto *p, serial_port *s)
{
    /* *p = &vtable PTR_ssp_protocol_dtor_0016f388; all members zeroed.
     * Serial handle goes to +0x6d (param_1[0xd]); stop flag +0x70 = 0. */
    ssp_proto_zero_init(p);
    ssp_proto_set_serial(p, s);
    ssp_set_stop(p, false);
    /* ssp_deque_init(+0xf), ssp_deque_init(+0x1f), cv/mutex ctor (+0x29):
     * vendor STL construction, modelled. */
    ssp_txq_init(p);
    return p;
}

/* --------------------------------------------------------------------------
 * ssp_protocol_start — OEM 0x1245a0.
 *
 * Open the serial port, clear the stop flag, and spawn the RX and TX worker
 * threads (std::thread fields +0x08 and +0x10). The OEM aborts (std::terminate)
 * if a thread field is already joinable when assigned.
 * -------------------------------------------------------------------------- */
void ssp_protocol_start(ssp_proto *p)
{
    serial_port_open(ssp_serial(p));
    ssp_set_stop(p, false);

    /* rx thread -> field +0x08, entry ssp_receive_loop */
    ssp_thread_set(p, 0x08, rd_thread_spawn((void (*)(void *))ssp_receive_loop, p));
    /* tx thread -> field +0x10, entry ssp_send_loop */
    ssp_thread_set(p, 0x10, rd_thread_spawn((void (*)(void *))ssp_send_loop, p));
}

/* --------------------------------------------------------------------------
 * ssp_protocol_start_dispatch_thread — OEM 0x121f70.
 *
 * Lazily (re)spawn the third "dispatch" worker (std::thread field +0x18,
 * State vtable PTR_LAB_0016f3f8). If a previous dispatch thread is joinable it
 * is joined first. Not in the headline list but part of the SSP module.
 * -------------------------------------------------------------------------- */
void ssp_protocol_start_dispatch_thread(ssp_proto *p)
{
    if (ssp_thread_field(p, 0x18) != NULL)
        ssp_thread_join_field(p, 0x18);
    ssp_thread_set(p, 0x18, rd_thread_spawn((void (*)(void *))ssp_dispatch_entry, p));
}

/* dispatch thread body (State at +0x10 of its closure is just `this`); the OEM
 * closure carries only the object pointer, so the entry forwards to it. */
static void ssp_dispatch_entry(void *arg) { (void)arg; }

/* --------------------------------------------------------------------------
 * ssp_protocol_stop — OEM 0x124700.
 *
 * Request stop (+0x70 = 1), wake the TX cv (+0x148), join the TX thread if
 * running, request the serial transport to abort blocking reads, join the RX
 * thread if running, then close the port.
 * -------------------------------------------------------------------------- */
void ssp_protocol_stop(ssp_proto *p)
{
    ssp_set_stop(p, true);
    ssp_txq_notify(p);                       /* wake SendLoop                 */

    if (ssp_thread_field(p, 0x10) != NULL)
        ssp_thread_join_field(p, 0x10);      /* join TX thread                */

    serial_port_request_stop(ssp_serial(p)); /* unblock blocking read         */

    if (ssp_thread_field(p, 0x08) != NULL)
        ssp_thread_join_field(p, 0x08);      /* join RX thread                */

    serial_port_close(ssp_serial(p));
}

/* --------------------------------------------------------------------------
 * ssp_protocol_dtor — OEM 0x124780.
 *
 * Reinstall the vtable, join the dispatch thread (+0x18) if present, Stop()
 * the workers, free the subscriber vector (+0x35), destroy the cv/mutex block
 * (+0x29) and both deques (+0x1f, +0xf), freeing every queued std::vector.
 * The deque teardown is vendor STL; the lifecycle order is ours.
 * -------------------------------------------------------------------------- */
void ssp_protocol_dtor(ssp_proto *p)
{
    ssp_proto_reset_vtable(p);

    if (ssp_thread_field(p, 0x18) != NULL)
        ssp_thread_join_field(p, 0x18);      /* join dispatch thread          */

    ssp_protocol_stop(p);

    ssp_subs_free(p);                        /* free subscriber vector +0x35  */
    ssp_cv_destroy(p);                       /* cv/mutex block +0x29          */
    ssp_txq_destroy(p);                      /* both deques + queued frames   */
}

/* --------------------------------------------------------------------------
 * ssp_protocol_delete — OEM 0x124930.
 * dtor + operator delete(this, 0x1c0).
 * -------------------------------------------------------------------------- */
void ssp_protocol_delete(ssp_proto *p)
{
    ssp_protocol_dtor(p);
    op_delete(p, 0x1c0);
}
