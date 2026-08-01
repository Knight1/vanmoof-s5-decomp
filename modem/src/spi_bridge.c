/*
 * spi_bridge.c -- VanMoof S5 modem (nRF9160) -- SPI-slave bridge to the i.MX8 (COBS/CRC16 framing + dispatch).
 *
 * Reconstructed (behaviour-oriented C) from the OEM image
 *   modem.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (Nordic nRF9160, Cortex-M33, Zephyr RTOS + nRF Connect SDK; MCUboot, base 0x0).
 * Only VanMoof application code is reconstructed; Zephyr (k_*/net_*/socket), the
 * nRF Connect SDK (lte_lc/nrf_modem/location/sms/dfu_target) and mbedTLS/newlib
 * are vendor (extern). OEM addresses are in each function header.
 */
#include "modem.h"


/* ===== spi_process_thread  (OEM 0x00012528) [confidence: medium] =====
 * Zephyr k_work_q thread body: waits for work items enqueued by the consumer thread (or other subsystems), dequeues each item and invokes its handler callback, manages reference counts and cancellation lists */
/* spi_process_thread — Zephyr work-queue dispatcher loop.
 * Registered via K_THREAD_DEFINE("spi_process_thread", ...) and started
 * by k_work_q_start().  Receives work items submitted by spi_consumer_thread
 * (and other subsystems) and executes their callbacks. */
void spi_process_thread(struct k_work_q *work_q)
{
    while (1) {
        struct k_work *work;
        /* ---- critical section: dequeue next work item ---- */
        unsigned int key;
        key = irq_lock();  /* setBasePriority(0x20) via BASEPRI */
        work = (struct k_work *)work_q->fifo.data_q.head; /* +0xf8 */
        if (work == NULL) {
            /* no work: drain any pending ready items */
            if (work_q->flags & K_WORK_QUEUE_FLAG_PLUGGED) { /* +0x110 bit1 */
                /* drain drain_list (k_work items at +0x108) one by one */
                int rc;
                do {
                    rc = k_sem_take(&work_q->drainer_sem, K_NO_WAIT); /* FUN_0003dbca */
                } while (rc != 0);
            }
            /* block until a new item arrives */
            k_thread_suspend_current(g_spi_process_thread_obj, key,
                                     &work_q->fifo, 0,
                                     g_spi_process_wait_q, 0); /* FUN_000310e8 */
            continue;
        }
        /* dequeue: work_q->fifo.data_q.head = work->node.next (+0x00) */
        work_q->fifo.data_q.head = (void *)work->node.next;
        if (work == (struct k_work *)work_q->fifo.data_q.tail) /* +0xfc */
            work_q->fifo.data_q.tail = work->node.next;
        /* mark work_q as busy */
        work_q->flags |= K_WORK_QUEUE_FLAG_BUSY; /* +0x110 bit1 */
        /* mark work item as RUNNING (clear QUEUED bit, set IN_PROGRESS bit 0) */
        work->flags = (work->flags & ~K_WORK_FLAG_QUEUED) | K_WORK_FLAG_RUNNING; /* +0x03 */
        irq_unlock(key);  /* restore BASEPRI */
        /* ---- invoke handler ---- */
        ((k_work_handler_t)work->handler)(work); /* +0x04 function pointer */
        /* ---- post-completion: update flags and notify cancellers ---- */
        key = irq_lock();
        if ((work->flags >> 2) & 1) {
            /* work item was cancelled while running — notify waiters */
            struct k_work_cancel_sync *cs;
            struct k_work_cancel_sync **list = (struct k_work_cancel_sync **)work_q->cancel_list_head; /* +0x00 of cancel table */
            while (list != NULL && *list != NULL) {
                struct k_work_cancel_sync *node = *list;
                if ((struct k_work *)node->work == work) {
                    /* unlink from cancel list */
                    struct k_work_cancel_sync *nxt = node->next;
                    if (cs == NULL)
                        *work_q->cancel_list_head = nxt;
                    else
                        cs->next = nxt;
                    node->next = NULL;
                    k_sem_give(&node->sem); /* FUN_00030768 */
                    cs = NULL;
                } else {
                    cs = node;
                }
                list = (struct k_work_cancel_sync **)&node->next;
            }
            /* clear IN_PROGRESS + CANCEL bits */
            work->flags &= ~(K_WORK_FLAG_RUNNING | K_WORK_FLAG_CANCELLING);
        } else {
            work->flags &= ~K_WORK_FLAG_RUNNING;
        }
        /* clear work_q busy flag; if fully empty trigger reschedule */
        work_q->flags &= ~K_WORK_QUEUE_FLAG_BUSY;
        irq_unlock(key);
        if (!(work_q->flags & K_WORK_QUEUE_FLAG_NO_YIELD))
            k_yield(); /* FUN_00030ed0 */
    }
}


/* ===== spi_publish  (OEM 0x00015e74) [confidence: high] =====
 * Core publish function: builds a SPI frame for a given control byte and channel, serialises CBOR payload, and calls spi_send_frame; the 'busy' flag prevents re-entrant sends; returns 0 on success */
/* spi_publish — encode and transmit a CBOR payload on the given SPI channel.
 * ctrl_byte = 0x81 for modem-ctrl (modem/* topics to i.MX8).
 * busy_flag is cleared before send to allow re-entrant callers to proceed. */
int spi_publish(uint8_t       ctrl_byte,   /* param_1, e.g. 0x81 */
                const void   *topic_key,   /* param_2, CBOR key literal in flash */
                const void   *cbor_payload,/* param_3, serialised CBOR buffer   */
                uint32_t      payload_len) /* param_4 */
{
    uint8_t frame_buf[0xa0];
    /* if another publish is in flight (busy flag non-zero), skip */
    if (spi_is_busy() && !force) /* FUN_00016be0 — reads *g_spi_busy */
        return 0;
    spi_clear_busy();            /* FUN_00016bec — *g_spi_busy = 0 */
    if (payload_len >= 0x97) {
        LOG_ERR("messages exceeds %d bytes. message is %d bytes", 0x96, payload_len);
        return -1;
    }
    /* build COBS frame: ctrl_byte | topic_key | payload */
    int enc_len = spi_frame_encode(frame_buf, sizeof(frame_buf),
                                   ctrl_byte, /*flags=*/0,
                                   topic_key, cbor_payload, payload_len); /* FUN_000169cc */
    if (enc_len < 0) {
        LOG_ERR("Failed publishing subscribe");
        return -1;
    }
    /* send via COBS+SPI */
    return spi_send_frame(frame_buf, (uint32_t)enc_len); /* FUN_00016a50 */
}


/* ===== spi_publish_modem  (OEM 0x00015f30) [confidence: high] =====
 * Sends a subscription announcement for a single channel ID on the modem-ctrl pipe (control byte 0x81, topic key 0xe001); called from the consumer thread for each entry in the subscribe table to inform i.MX8 which topics the modem handles */
/* spi_publish_modem — announce subscription for channel channel_id on 0x81.
 * Wraps spi_publish(0x81, TOPIC_SUBSCRIBE, ...). */
int spi_publish_modem(uint16_t channel_id)
{
    uint8_t  cbor_payload[0x92];
    uint32_t cbor_len = 0;
    /* build minimal CBOR map { "channel": channel_id } using TinyCBOR */
    memset(cbor_payload, 0, sizeof(cbor_payload)); /* FUN_0003e3a6 */
    void *enc_ctx[4];
    cbor_encoder_init(enc_ctx, cbor_payload, sizeof(cbor_payload)); /* FUN_0003b872 */
    cbor_encoder_append_uint(enc_ctx, 0, channel_id, 0);            /* FUN_0003b87c */
    cbor_len = cbor_encoder_get_buffer_size(enc_ctx);               /* embedded in FUN_0003b972 */
    /* publish on modem-ctrl channel (control byte 0x81) */
    int rc = spi_publish(0x81, &TOPIC_SUBSCRIBE, cbor_payload,
                         cbor_len); /* FUN_00015e74 */
    if (rc != 0)
        LOG_ERR("Failed publishing subscribe");
    return rc;
}


/* ===== spi_subscribe_dispatch  (OEM 0x00015fa8) [confidence: medium] =====
 * Incoming subscribe-request dispatcher: decodes a CBOR map from i.MX8 containing a channel ID (integer or hex string), looks up matching entries in the subscribe dispatch table, and calls spi_publish_modem for each match; also iterates the command dispatch table to invoke handlers */
/* spi_subscribe_dispatch — handle an incoming 'subscribe' message from i.MX8.
 * Called indirectly via the spi_process_thread work callback.
 * param_3 = reply channel context; param_4/param_5 = raw CBOR bytes. */
void spi_subscribe_dispatch(void *ctx,
                            void *unused1,
                            void *reply_ch,
                            const void *cbor_in,
                            size_t      cbor_in_len)
{
    /* TinyCBOR parse the incoming map */
    CborParser  parser;
    CborValue   map_it, key_it;
    cbor_parser_init(cbor_in, cbor_in_len, 0, &parser, &map_it); /* FUN_0003bd18 */
    if (cbor_value_get_type(&map_it) != CborMapType)  /* local_62 != -0x60 */
        goto error;
    uint32_t channel_id = 0;
    /* try integer key first: map["ftp"] or map[integer] */
    int rc = cbor_value_map_find_value(&map_it, g_key_channel, &key_it); /* FUN_00033ee0 */
    if (rc == 0) {
        channel_id = (uint32_t)key_it.extra;
    } else {
        /* try hex string */
        CborValue hex_val;
        rc = cbor_value_map_find_value(&map_it, g_key_channel, &hex_val); /* FUN_00033f1a */
        if (rc != 0) {
            LOG_ERR("Invalid format for update-request");
            goto error;
        }
        uint64_t tmp;
        cbor_decode_hex(&hex_val, &tmp); /* FUN_00010d18 */
        channel_id = (uint32_t)tmp;
    }
    /* optional: re-subscribe all channels if 'command' key present */
    char cmd_flag[16];
    cbor_value_copy_text_string(&map_it, g_key_command, cmd_flag, NULL); /* FUN_00033f50 */
    if (cmd_flag[0] != '\0') {
        /* iterate the publish-table and (re)subscribe each channel */
        const struct pub_entry *p = g_publish_table_start;
        const struct pub_entry *pe = g_publish_table_end;
        for (; p < pe; p++) {
            int rc2 = spi_publish_modem(p->channel_id); /* FUN_00015f30 */
            LOG_DBG("subscribe channel=%u rc=%d", p->channel_id, p->fn_name, rc2);
        }
    }
    /* iterate subscribe table: call matching handler */
    const struct sub_entry *s = g_subscribe_table_start; /* DAT_00016124 */
    const struct sub_entry *se = g_subscribe_table_end;  /* DAT_00016128 */
    LOG_DBG("Dispatching subscribe for channel %u", channel_id & 0xffff);
    for (; s < se; s++) {
        if (s->channel_id == (uint16_t)channel_id) {
            s->handler(s->ctx, (uint16_t)channel_id, reply_ch); /* fn ptr +2, ctx +4 */
        }
    }
    return;
error:
    LOG_ERR("Failed setting up CBOR-parser");
}


/* ===== spi_consumer_thread  (OEM 0x00016410) [confidence: high] =====
 * SPI slave receive loop: reads SPI frames from i.MX8, validates sync/length/CRC16, COBS-decodes payload, enqueues decoded messages via k_msgq_put, sends subscription announcements via spi_publish_modem, blocks on k_poll for next transaction */
/* spi_consumer_thread — nRF9160 SPI-slave receive + dispatch loop.
 * Registered via K_THREAD_DEFINE("spi_consumer_thread", ...). */
void spi_consumer_thread(void *p1, void *p2, void *p3)
{
    /* 0xc1e-byte DMA-aligned rx staging area (double-buffered: 2 × 0x7DE) */
    uint8_t  rx_staging[0xc1e];
    /* SPI config: CPOL=0/CPHA=0, freq from DTS; tx buf reused across calls  */
    struct spi_config  spi_cfg;   /* local, init once */
    struct spi_buf     tx_buf[1], rx_buf[1];
    struct spi_buf_set tx_set, rx_set;
    struct ring_buf   *cobs_ring  = g_cobs_ring;   /* FUN_0003e248/FUN_0003e2b6 ring */
    const struct device *spi_dev  = g_spi_slave_dev;
    const struct device *spi_dev2 = g_spi_slave_dev2;
    uint8_t  *tx_frame = g_tx_frame_buf; /* 0x7de-byte double-buffer */
    uint16_t *buf_idx  = &g_tx_buf_idx;
    /* Subscribe-table: array of { channel_id u16, handler_fn, ctx } entries */
    const struct sub_entry *sub_start = g_subscribe_table_start;
    const struct sub_entry *sub_end   = g_subscribe_table_end;
    /* Decoded-message output buffer (up to 0x40a bytes) */
    uint8_t  decoded[0x40a];
    uint8_t  rx_tmp[4];            /* COBS output fragment */
    /* ---- one-shot SPI config ---- */
    memcpy(rx_staging, g_rx_staging_src, 0xc1e); /* FUN_0003e078 */
    /* clear poll-event array */
    memset(&g_poll_events, 0, sizeof(g_poll_events));
    /* point spi_cfg at SPI slave device, clear sync counter */
    g_tx_frame_ptr = g_tx_frame_buf;
    /* configure SPI GPIO direction via driver vtable */
    *(g_spi_dev->config->ops->gpio_config) &= ~1;
    /* first async transceive to prime the SPIS FIFOS */
    ((spi_transceive_t)spi_cfg.ops->transceive)(spi_dev, (uint32_t)0x60000);
    /* mark tx frame as type=0x07/0xcf */
    tx_frame[0xa] = 7;
    tx_frame[0xb] = 0xcf;
    /* set up spi_buf / spi_buf_set for the 0x7de-byte half-buffer */
    tx_buf[0].buf = tx_frame;
    tx_buf[0].len = 0x7de;
    rx_buf[0].buf = g_rx_alt_buf;
    rx_buf[0].len = 0x7de;
    tx_set = (struct spi_buf_set){ tx_buf, 1 };
    rx_set = (struct spi_buf_set){ rx_buf, 1 };
    tx_frame[0xc] = tx_frame[0xd] = 0;
    *buf_idx = 0;
    /* ---- main loop ---- */
    while (1) {
        /* --- inner retry loop: async SPI transceive, up to 10 tries --- */
        int retries = 10;
        int rc;
        do {
            rc = ((spi_async_t)spi_dev2->api->transceive_async)(
                     spi_dev2, &g_spi_cfg, &tx_set, &rx_set);
            if (rc != 0)
                LOG_ERR("spi_transceive_async returns: (%d)", rc); /* FUN_00033eae */
            retries--;
        } while (rc != 0 && retries > 0);
        /* --- frame processing --- */
        uint32_t frame_len = g_spi_frame_len;
        if (frame_len == 0)
            goto poll;  /* nothing received */
        /* parse frame: first byte is frame type */
        uint8_t  *rx = g_rx_alt_buf + *buf_idx * 0x7de;
        uint8_t   ftype  = rx[0];
        uint16_t  payload_len;
        if (ftype == 0x01) {
            /* ---- type 0x01: full data frame ---- */
            /* check minimum header: at least 7 bytes before CRC sentinel */
            if (frame_len <= 6) goto next;
            uint32_t magic = *(uint32_t *)(rx + frame_len - 6);
            if (magic != SYNC_MAGIC) {                 /* 0x55AA55AA? */
                LOG_WRN("Invalid sync_bytes: 0x%08x", magic);
                goto next;
            }
            uint16_t crc_rcvd = *(uint16_t *)(rx + frame_len - 2);
            uint16_t crc_calc = crc16_reflect(0xffff, rx, frame_len - 2); /* FUN_0003404c */
            crc_calc = __builtin_bswap16(crc_calc);
            if (crc_calc != crc_rcvd) {
                LOG_WRN("Invalid CRC, calculated 0x%04x, received 0x%04x",
                        crc_calc, crc_rcvd);
                goto next;
            }
            /* feed raw bytes into COBS ring buffer (FUN_0003e248) */
            int remaining = (int)frame_len - 7;
            while (remaining > 0) {
                int wrote = ring_buf_put_claim(cobs_ring, rx + 1, remaining); /* FUN_0003e248 */
                if (wrote < 0) {
                    /* ring full: flush it (FUN_0003e078) */
                    if (ring_buf_is_empty(cobs_ring)) {  /* FUN_0003e2b6 */
                        LOG_ERR("Failed adding raw-data to the input-stream, resetting");
                        goto frame_error;
                    }
                    ring_buf_reset(cobs_ring, cobs_ring->buf, cobs_ring->size); /* FUN_0003e078 */
                    continue;
                }
                /* COBS decode each full frame in the ring */
                while (ring_buf_is_ready(cobs_ring)) {  /* FUN_0003e2b6 */
                    int dec_rc = cobs_decode(cobs_ring, decoded, sizeof(decoded), rx_tmp); /* FUN_0003e162 */
                    if (dec_rc != 0) {
                        LOG_ERR("Failed adding raw-data to the input-stream");
                        ring_buf_reset(cobs_ring, cobs_ring->buf, cobs_ring->size);
                    }
                    /* dispatch decoded packet to message queue */
                    rc = k_msgq_put(g_spi_msgq, decoded, &CBOR_SPI_TYPE, K_NO_WAIT); /* FUN_0003008c */
                    if (rc != 0)
                        LOG_WRN("Failed dispatching decoded SPI-message, err %d", rc);
                }
                remaining -= wrote;
            }
        } else if (ftype == 0x10) {
            /* ---- type 0x10: size/flush command ---- */
            uint16_t new_len = __builtin_bswap16(*(uint16_t *)(rx + 1));
            if (new_len > 0x7de) {
                LOG_WRN("messages exceeds %d bytes. message is %d bytes", 0x7de, new_len);
                goto next;
            }
            if (new_len < payload_len) {
                /* shift buffered tx data down */
                uint8_t *dst = g_tx_data_buf;
                memmove(dst, dst + new_len, payload_len - new_len);
                payload_len -= new_len;
                goto next;
            }
            /* accumulate new tx data via k_pipe_get (FUN_0003d5c8) */
            uint32_t got = 0;
            k_pipe_get(g_tx_pipe, tx_frame + payload_len + 0xe,
                       0x7ca - payload_len, &got,
                       1, K_NO_WAIT); /* FUN_0003d5c8 */
            payload_len += (uint16_t)got;
        }
        /* update tx frame length field (big-endian) */
        *(uint16_t *)(tx_frame + 0xc) = __builtin_bswap16(payload_len);
        if (payload_len != 0) {
            /* append sync sentinel + CRC16 */
            tx_frame[payload_len + 0xe + 0] = 0x55;
            tx_frame[payload_len + 0xe + 1] = 0xAA;
            tx_frame[payload_len + 0xe + 2] = 0x55;
            tx_frame[payload_len + 0xe + 3] = 0xAA;
            uint16_t crc = crc16_reflect(0xffff, tx_frame, payload_len + 0x12); /* FUN_0003404c */
            *(uint16_t *)(tx_frame + payload_len + 0xe + 4) = __builtin_bswap16(crc);
        }
        /* advance double-buffer index */
        *buf_idx = (*buf_idx + 1) % 2;
        /* notify SPI layer that TX frame is ready (FUN_00033eb4) */
        rc = spi_slave_tx_ready(*g_tx_frame_ptr, payload_len != 0);
        if (rc != 0)
            LOG_ERR("spi_transceive_async");
        /* send modem subscribe announcements to i.MX8 */
        for (const struct sub_entry *e = sub_start; e < sub_end; e++) {
            spi_publish_modem(e->channel_id);  /* FUN_00015f30 */
        }
        continue;
frame_error:
        goto next;
next:
        ; /* nothing */
poll:
        /* wait for next SPI transaction (k_poll with 1 event, infinite timeout) */
        k_poll(&g_poll_events, 1, K_FOREVER); /* FUN_000315b0 */
        /* reload next half-buffer pointer for the upcoming SPI call */
        uint32_t next_idx = (*buf_idx + 1) % 2;
        g_rx_alt_ptr = g_rx_staging + next_idx * 0x7de;
        tx_frame[0xc] = tx_frame[0xd] = 0;
    }
}


/* ===== spi_frame_encode  (OEM 0x000169cc) [confidence: high] =====
 * SPI frame header builder: serialises control byte, channel tag, big-endian payload length, CRC16 (big-endian), and payload copy into a flat output buffer; returns total frame size or 0xffffffff on overflow (>0x400 bytes) */
/* spi_frame_encode — build an outbound SPI frame in out_buf.
 * Frame layout (little-endian host, big-endian wire):
 *   [0]     ctrl_byte  (e.g. 0x81 = modem-ctrl)
 *   [1..2]  channel    (big-endian u16)
 *   [3]     flags
 *   [4..7]  payload_len (big-endian u32)
 *   [8..9]  crc16 of payload (big-endian)
 *   [10..]  payload copy
 * Returns total encoded size, or 0xffffffff on error. */
uint32_t spi_frame_encode(uint8_t *out_buf,    /* param_1 */
                          uint32_t out_size,   /* param_2 — must hold len+10 */
                          uint8_t  ctrl_byte,  /* param_3 */
                          uint8_t  flags,      /* param_4 */
                          uint16_t channel,    /* param_5 — stored big-endian */
                          const void *payload, /* param_6 */
                          uint32_t  len)       /* param_7 */
{
    uint32_t total = len + 10;
    if (total > 0x400) {
        LOG_ERR("Cannot fit publish data within SPI message"); /* FUN_00033eae */
        return 0xffffffff;
    }
    out_buf[0]  = ctrl_byte;
    *(uint16_t *)(out_buf + 1) = __builtin_bswap16(channel);
    out_buf[3]  = flags;
    *(uint32_t *)(out_buf + 4) = __builtin_bswap32(len);
    uint16_t crc = crc16_reflect(0xffff, payload, len); /* FUN_0003404c */
    *(uint16_t *)(out_buf + 8) = __builtin_bswap16(crc);
    memcpy(out_buf + 10, payload, len);                 /* FUN_0003e332 */
    /* swap channel to big-endian already done above; re-swap fields */
    *(uint16_t *)(out_buf + 1) = __builtin_bswap16(*(uint16_t *)(out_buf + 1));
    *(uint32_t *)(out_buf + 4) = __builtin_bswap32(*(uint32_t *)(out_buf + 4));
    *(uint16_t *)(out_buf + 8) = __builtin_bswap16(*(uint16_t *)(out_buf + 8));
    return total;
}


/* ===== spi_send_frame  (OEM 0x00016a50) [confidence: high] =====
 * SPI message sender: COBS-encodes the built frame into the TX pipe, then calls spi_transceive (via device lock/unlock) up to 10 times until the frame is sent; returns 0 on success or negative errno */
/* spi_send_frame — COBS-encode frame_buf[0..frame_len) and transmit via SPI.
 * Retries up to 10 times if the SPI layer returns -EAGAIN (-11). */
int spi_send_frame(const void *frame_buf,   /* param_1 = CBOR/encoded payload */
                   uint32_t    frame_len)   /* param_2 = output of spi_frame_encode */
{
    static uint8_t cobs_buf[0x406]; /* g_cobs_tx_buf */
    uint32_t cobs_len;
    /* lock SPI device for exclusive access */
    int rc = k_mutex_lock(g_spi_mutex, K_FOREVER);  /* FUN_000301f0 */
    if (rc != 0) {
        LOG_ERR("Failed publishing subscribe");
        return -ENODEV;
    }
    /* COBS-encode the frame */
    rc = cobs_encode(frame_buf, frame_len, cobs_buf, sizeof(cobs_buf)); /* FUN_0003e13e */
    if (rc < 0) {
        LOG_ERR("Failed encoding SPI-message with error-code %d", rc);
        goto out;
    }
    cobs_len = (uint32_t)rc;
    /* retry SPI transceive up to 10 times */
    int tries = 10;
    do {
        rc = k_pipe_put(g_spi_tx_pipe, cobs_buf, cobs_len, NULL); /* FUN_0003056c */
        tries--;
        if (tries == 0) break;
    } while (rc == -EAGAIN);
    if (rc != 0) {
        LOG_ERR("after 10 tries Failed appending encoded SPI-message to the pipe, %d", rc);
        spi_slave_tx_notify(); /* FUN_00016404 */
        rc = -ECOMM;
        goto out;
    }
    spi_slave_tx_notify(); /* FUN_00016404 — signal consumer thread */
    rc = 0;
out:
    k_mutex_unlock(g_spi_mutex); /* FUN_000302e0 */
    return rc;
}


/* ===== spi_slave_init  (OEM 0x00016bf8) [confidence: medium] =====
 * SPI slave device initialisation: configures the nrfx_spis peripheral via the Zephyr SPI driver, starts the first async transceive to arm the SPIS FIFO ready for i.MX8 traffic */
/* spi_slave_init — configure SPI slave and prime first async transfer.
 * Called once at system startup (SYS_INIT or k_thread_create entry). */
void spi_slave_init(void)
{
    /* pin-mux / GPIO config via device driver */
    int rc = gpio_pin_configure(g_spi_slave_dev, 0); /* FUN_0003d2cc — GPIO configure */
    if (rc != 0)
        LOG_ERR("GPIO Pin set error failure");
    /* start first SPI async transceive; the consumer thread polls for completion */
    rc = spi_transceive_async(g_spi_slave_dev,       /* FUN_0001c020 */
                              &g_spi_async_sig);
    if (rc != 0)
        LOG_ERR("SPI transceiver start error (%d)", rc);
    /* (no return value — errors are logged, not propagated) */
}


/* ===== cobs_encode_to_ring  (OEM 0x0003e094) [confidence: medium] =====
 * COBS encoder: encodes a flat byte buffer into COBS format and appends the encoded bytes (including framing 0x00 sentinel) into the SPI output ring buffer */
/* cobs_encode_to_ring — COBS-encode src[0..len) into the ring buffer.
 * Vendor/library code (do not reconstruct). */
extern int cobs_encode_to_ring(struct ring_buf *rb, const uint8_t *src, size_t len);


/* ===== cobs_decode_from_ring  (OEM 0x0003e162) [confidence: medium] =====
 * COBS decoder: reads COBS-encoded bytes from the SPI input ring buffer and writes decoded payload into the destination buffer; returns 0 on success, non-zero on framing error */
/* cobs_decode_from_ring — COBS-decode one packet from ring buffer rb
 * into out[0..out_size).  *actual_len set to decoded length.
 * Vendor/library code (do not reconstruct). */
extern int cobs_decode_from_ring(struct ring_buf *rb, uint8_t *out, uint32_t out_size,
                                 uint8_t *scratch);
