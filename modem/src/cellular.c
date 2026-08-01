/*
 * cellular.c -- VanMoof S5 modem (nRF9160) -- GNSS/location + SMS receive + network time.
 *
 * Reconstructed (behaviour-oriented C) from the OEM image
 *   modem.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (Nordic nRF9160, Cortex-M33, Zephyr RTOS + nRF Connect SDK; MCUboot, base 0x0).
 * Only VanMoof application code is reconstructed; Zephyr (k_*/net_*/socket), the
 * nRF Connect SDK (lte_lc/nrf_modem/location/sms/dfu_target) and mbedTLS/newlib
 * are vendor (extern). OEM addresses are in each function header.
 */
#include "modem.h"


/* ===== modem_app_event_handler  (OEM 0x00019788) [confidence: medium] =====
 * Main VanMoof modem application event handler — processes GNSS location fix events, publishes Google Maps URLs via SPI/MQTT, handles LTE status changes, SMS registration callbacks, and Location library init errors. Carved from gap 0x19780–0x1a07b; Ghidra missed it because its literal pool (at 0x19780) sits in orphaned data immediately before the PUSH prologue at 0x19788. */
/* 0x00019788 — modem app event handler */
void modem_app_event_handler(uint conn_handle)
{
    /* Zephyr: FUN_00035210 = k_sem_take / flag-set; FUN_00035226 = k_sem_give / flag-clear */
    /* On GNSS/location result, log Google Maps URL and forward to SPI bridge */
    if (/* location_event == GOT_FIX */ 0) {
        /* s_Got_location___Google_maps_URL__h_00041e93:
           "Got location - Google maps URL: https://maps.google.com/?q=%.06f,%.06f" */
        LOG_INF(s_Got_location___Google_maps_URL__h_00041e93, lat, lon);
        /* publish location via SPI→MQTT */
    }
    /* s_Getting_location_timed_out_00041eee = "Getting location timed out" */
    /* On Location library init failure: */
    /* s_0x41f65 = " the Location library failed, error: %d\n" */
    if (/* location_init_err != 0 */ 0) {
        LOG_ERR("Initializing the Location library failed, error: %d\n",
                /* err */ 0);
    }
}


/* ===== sms_event_dispatch  (OEM 0x0001af08) [confidence: medium] =====
 * SMS / modem event dispatcher — walks a linked list of registered handler entries (each storing a filter ID, an initialiser callback, and a handler callback), and for the matching entry calls both callbacks. Used to route incoming SMS messages and modem events to VanMoof app handlers (sms_callback, lte_handler, etc.). */
/* 0x0001af08 — SMS/modem event dispatcher */
int sms_event_dispatch(int event_id)
{
    /* DAT_0001af4c = handler_list_end, DAT_0001af50 = handler_list_start */
    entry_t *entry = DAT_0001af4c;  /* pointer to first entry */
    entry_t *end   = DAT_0001af50;
    while (entry < end) {
        if (entry->filter_id == event_id || entry->filter_id == 0) {
            /* call entry->init_cb — sets up context for this event */
            int rc = ((init_cb_t)entry->init_cb)(event_id);
            if (rc != 0) {
                /* indirect dispatch — entry->handler_cb(event_id) */
                return ((handler_cb_t)entry->handler_cb)(event_id);
            }
        }
        entry += 4;   /* each entry is 4 words = 16 bytes */
    }
    /* errno = EBADF (106 = 0x6a) */
    *thunk_FUN_0002f9c8() = 0x6a;
    return -1;
}


/* ===== sms_pdu_encode_gsm7  (OEM 0x0001e17c) [confidence: high] =====
 * GSM 7-bit SMS PDU encoder — encodes a byte buffer into GSM septet-packed format, calling FUN_000374e6 for each block of 3 bytes. Used by the VanMoof modem firmware to build outgoing SMS PDUs for anti-theft alerts. Takes payload pointer, length, and socket-write callback. */
/* 0x0001e17c — GSM 7-bit PDU encoder */
void sms_pdu_encode_gsm7(uint8_t *payload, int has_smsc, int len,
                          uint16_t tp_pid_dcs, write_fn_t write_cb,
                          uint32_t cb_arg, int *bytes_encoded_out)
{
    uint16_t hdr = has_smsc ? 0x0906 : 0x1404;
    /* write 2-byte header (TP-UDL + TP-UDHL) */
    int rc = write_cb(&stack_header, 2, cb_arg, hdr,
                      (uint16_t)((uint16_t)payload);
    if (rc != 0) return;

    int i = 0;
    if (has_smsc) {
        /* write 3-byte SMSC field */
        rc = sms_pdu_write_block(&smsc_3byte, 3, write_cb, cb_arg);
        if (rc != 0) return;
        i = 1;
    }
    int limit = len - i;
    if (limit > (has_smsc ? 0x5a : 0x5d)) limit = (has_smsc ? 0x5a : 0x5d);
    /* pack remaining bytes in 3-byte blocks via FUN_000374e6 */
    for (; i + 2 < limit; i += 3) {
        rc = FUN_000374e6(&payload[i], 3, write_cb, cb_arg);
        if (rc != 0) return;
    }
    /* handle 1 or 2 trailing bytes */
    /* ... padding + final write ... */
    /* write PDU terminator */
    rc = write_cb(DAT_0001e2b8, 1, cb_arg);
    if (rc == 0) *bytes_encoded_out = i;
}


/* ===== gnss_at_response_stub  (OEM 0x0001f698) [confidence: low] =====
 * Minimal wrapper that immediately tail-calls FUN_0001f368; in Ghidra the reachable block at 0x1f6c4 is marked unreachable. Likely a small thunk generated for a Zephyr AT notification path — sits adjacent to literal pools referencing 'nrf_modem_at_scanf() returned error: %' (0x4776d) and GNSS-related error strings. */
/* 0x0001f698 — AT response stub / thunk */
void gnss_at_response_stub(void)
{
    /* WARNING: Removing unreachable block (ram,0x0001f6c4) */
    FUN_0001f368();
}


/* ===== gnss_location_request  (OEM 0x000201a8) [confidence: medium] =====
 * GNSS / location request handler — iterates a list of location method structures; for each method with a valid handler pointer, calls the handler (GNSS PVT, cell-based, etc.); if no method has a result, logs an error via FUN_000180ac; on success, sets the active-fix flags in the GNSS state object (DAT_000202d0/d4) and signals a work queue item via FUN_0003d99e. */
/* 0x000201a8 — GNSS location request */
void gnss_location_request(void)
{
    /* FUN_0001f8dc = location_request_start (sets fix-in-progress flags) */
    FUN_0001f8dc(0xffffffff, 0xffffffff);
    int method_count = 0;
    FUN_000394c2();  /* vendor: k_mutex_lock / nrf_modem_gnss_start fence */

    /* iterate location methods array [DAT_000202bc .. DAT_000202e0] */
    for (uint8_t *method = DAT_000202bc;
         method < DAT_000202e0;
         method += 0x2c, method_count++) {
        int *handler = *(int **)(**(int **)method + 8);
        if (handler == NULL || *handler == 0) {
            /* method not ready — log error if log level permits */
            if ((*DAT_000202c0 & 7) != 0) {
                /* error: method not active */
                FUN_000180ac(DAT_000202c0, &DAT_00001840,
                             &(struct){3, DAT_000202e4, method}, 0);
            }
        } else {
            FUN_00038484(*(int **)method, 6); /* set method state = 6 */
            (*handler)(method);              /* invoke method handler */
        }
    }
    if (method_count == 0) {
        /* no active methods — log warning */
        FUN_000180ac(DAT_000202c0, &DAT_00001040,
                     &(struct){2, DAT_000202c4}, 0);
    } else if (method_count > 2 && (*DAT_000202c0 & 6) != 0) {
        /* multiple methods — log count */
        FUN_000180ac(DAT_000202c0, 0x2080,
                     &(struct){4, DAT_000202c8, method_count + 2,
                                method_count}, 0);
    }
    /* mark fix-state bytes active */
    *(uint8_t *)(DAT_000202d0 + 0x38) = 0x40;
    *(uint8_t *)(DAT_000202d0 + 0x78) = 0x40;
    /* schedule location work item */
    FUN_0003d99e(DAT_000202d8, DAT_000202d4);
    /* clear result cache */
    *DAT_000202dc = 0;
    DAT_000202dc[1] = 0;
    FUN_0001f8ec();  /* vendor: k_mutex_unlock */
}


/* ===== sms_send_pdu  (OEM 0x000235ac) [confidence: high] =====
 * VanMoof SMS send entry point — wraps the GSM 7-bit PDU encoder (sms_pdu_encode_gsm7) with a retry loop, computing a CRC/checksum via FUN_0003404c and writing PDU blocks over the modem socket until the full payload is encoded or an error occurs. */
/* 0x000235ac — SMS PDU send loop */
int sms_send_pdu(uint8_t *buf, int len)
{
    /* FUN_0003404c computes CRC or address for the PDU */
    uint32_t crc_or_addr = FUN_0003404c(0, buf, len);
    int encoded = 0;
    int remaining = len;
    while (remaining != 0) {
        /* write_fn stored in DAT_000235b4 (socket write wrapper) */
        int rc = sms_pdu_encode_gsm7(buf, encoded == 0 /*has_smsc*/,
                                     remaining, crc_or_addr,
                                     (write_fn_t)DAT_000235b4, 0,
                                     &encoded);
        if (rc != 0) return rc;
        buf       += encoded;
        remaining -= encoded;
    }
    return 0;
}


/* ===== sms_send_dispatch  (OEM 0x0003706e) [confidence: medium] =====
 * Low-level SMS dispatch thunk — extracts the payload pointer and length from a message buffer (param_2[+8], param_2[+0xc]) and calls sms_send_pdu, then frees the message buffer via thunk_FUN_0001e704. */
/* 0x0003706e — SMS message dispatch */
int sms_send_dispatch(uint32_t param_1, sms_msg_t *msg,
                      uint32_t param_3, uint32_t param_4)
{
    int rc = sms_send_pdu((uint8_t *)(msg->payload),
                          (uint16_t)(msg->length),
                          param_3, param_4, param_4);
    /* free the message buffer */
    thunk_FUN_0001e704(msg);
    return rc;
}
