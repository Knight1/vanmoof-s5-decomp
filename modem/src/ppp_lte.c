/*
 * ppp_lte.c -- VanMoof S5 modem (nRF9160) -- PPP IP bearer + LTE-M/NB-IoT link control.
 *
 * Reconstructed (behaviour-oriented C) from the OEM image
 *   modem.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (Nordic nRF9160, Cortex-M33, Zephyr RTOS + nRF Connect SDK; MCUboot, base 0x0).
 * Only VanMoof application code is reconstructed; Zephyr (k_*/net_*/socket), the
 * nRF Connect SDK (lte_lc/nrf_modem/location/sms/dfu_target) and mbedTLS/newlib
 * are vendor (extern). OEM addresses are in each function header.
 */
#include "modem.h"


/* ===== lte_link_event_handler  (OEM 0x000132b8) [confidence: medium] =====
 * MQTT subscription handler for LTE modem events (CEREG / AT+CGEREP=1 notifications). On registration status change: if unregistered calls lte_lc_offline; if registered calls ppp_carrier_update. Also processes IP address changes via net_if callbacks. */
/* OEM addr 0x132b8 – LTE link event handler
 * Subscribed to MQTT topics: cereg_change, ip_addr_change */
void lte_link_event_handler(void)
{
    /* Decode incoming SPI/MQTT frame */
    struct mqtt_payload pay;
    spi_frame_decode(&pay, /*in_r3*/0, /*stack*/0);

    struct cbor_map outer, inner;
    if (cbor_decode_map(&pay, &outer) != 0 || outer.type != CBOR_TYPE_MAP)
        goto parse_error;

    /* Registration status field */
    struct cbor_item reg_item;
    if (cbor_map_get(&outer, g_key_cereg_stat, ®_item) == 0 &&
        reg_item.type == CBOR_TYPE_UINT) {
        if (reg_item.u16 == 0) {
            LOG_DBG("LTE: not registered – going offline");
            lte_lc_offline();       /* FUN_00027b34(4) */
        } else {
            LOG_DBG("LTE: registered");
            ppp_carrier_update(g_ppp_iface_token);  /* FUN_00027d88 */
        }
    }

    /* IP address active field */
    struct cbor_item ip_item;
    cbor_map_get(&outer, g_key_ip_active, &inner);
    if (cbor_map_get(&outer, g_key_ip_addr, &ip_item) == 0 &&
        ip_item.type == CBOR_TYPE_UINT) {
        /* net_mgmt IP up/down callback */
        net_if_addr_changed(ip_item.u16 != 0);  /* FUN_0002727c */
    }
    return;

parse_error:
    LOG_ERR("AT+CGEREP=1 failed, err %d\n", -EBADMSG);
}


/* ===== ppp_state_dispatch  (OEM 0x000133a8) [confidence: high] =====
 * MQTT message handler for PPP control: reads a 'start' flag from the payload; if set calls ppp_carrier_on, otherwise calls ppp_sockets_close. */
/* OEM addr 0x133a8 – MQTT ppp_ctrl handler (start/stop) */
void ppp_state_dispatch(int mqtt_msg_param)
{
    /* param_1+0x10 holds the 'start' flag from the parsed JSON payload */
    if (*(char *)(mqtt_msg_param + 0x10) != '\0') {
        /* PPP start requested */
        LOG_DBG("PPP start");
        ppp_carrier_on();
    } else {
        /* PPP stop requested */
        LOG_DBG("PPP stop");
        ppp_sockets_close();
    }
}


/* ===== modem_lte_config_handler  (OEM 0x0001385c) [confidence: medium] =====
 * MQTT handler for the 'modem_lte_config' subscription topic. Issues AT+CGCONTRDP=0 to query PDP context parameters, encodes the result (APN, IP, mask) into a CBOR/SPI reply, and publishes it back to the i.MX8 via the SPI MQTT bridge. */
/* OEM addr 0x1385c – modem_lte_config MQTT handler.
 * Reads PDP context 0 parameters and publishes them. */
void modem_lte_config_handler(void)
{
    /* AT+CGCONTRDP=0: query PDP context 0 */
    char apn_buf[100];     /* +0x71a in context struct */
    char mask_buf[100];    /* +0x78a */
    char gw_buf[100];      /* +0x8da */

    int apn_len  = nrf_modem_at_scanf(*(uint8_t *)(g_ctx + 0x784), g_ctx + 0x71a,  100);
    int mask_len = nrf_modem_at_scanf(*(uint8_t *)(g_ctx + 0x7f4), g_ctx + 0x78a,  100);
    int gw_len   = nrf_modem_at_scanf(*(uint8_t *)(g_ctx + 0x944), g_ctx + 0x8da,  100);

    if (-(apn_len >> 31) - (mask_len >> 31) != (gw_len >> 31)) {
        /* Lengths inconsistent – error path */
        LOG_ERR("Modem could not be connected, error: %d", gw_len);
        spi_publish_error(g_spi_ctx, g_err_str, 2);
        return;
    }

    /* Build CBOR map: {apn, mask, gw} */
    struct cbor_encoder enc;
    uint8_t cbor_buf[32];
    cbor_encoder_init(&enc, cbor_buf, sizeof(cbor_buf));
    cbor_map_open(&enc, g_lte_config_map_hdr);
    cbor_encode_str(&enc, g_key_apn);
    cbor_encode_str_raw(&enc, g_ctx + 0x8da);
    cbor_encode_str(&enc, g_key_ip);
    cbor_encode_str_raw(&enc, g_ctx + 0x71a);
    cbor_encode_str(&enc, g_key_mask);
    cbor_encode_str_raw(&enc, g_ctx + 0x78a);
    int enc_len = cbor_map_close(&enc, cbor_buf);

    if (enc_len == 0) {
        /* Send reply via SPI to i.MX8 topic 0xfa01 */
        int err = spi_mqtt_publish(0x81, &g_topic_0xfa01, cbor_buf,
                                   *(uint32_t *)(enc.output_buf + 4));
        if (err == 0) return;
        if ((*g_log_ctx & 7) == 0) return;
        LOG_ERR("Modem could not be connected, error: %d", err);
        spi_publish_error(g_spi_ctx, g_err_str, 3);
    } else {
        /* Encoding error */
        spi_publish_error(g_spi_ctx, g_enc_err_str, 2);
    }
}


/* ===== modem_nordic_update_config_handler  (OEM 0x000139b8) [confidence: medium] =====
 * MQTT handler for the 'modem_nordic_update_config' subscription topic. Parses a CBOR payload containing nRF modem firmware update configuration (type=0 → full update, type non-zero → partial). Stores parsed fields and acknowledges via SPI. */
/* OEM addr 0x139b8 – modem_nordic_update_config MQTT handler */
void modem_nordic_update_config_handler(char *payload)
{
    if (payload == NULL) {
        /* No payload: log and return */
        if ((*g_log_ctx & 7) == 0) return;
        LOG_WRN("modem_nordic_update_config: NULL payload");
        spi_publish_error(g_spi_ctx, g_err_str, 2);
        return;
    }

    /* Build response CBOR map */
    struct cbor_encoder enc;
    uint8_t cbor_buf[260];
    cbor_encoder_init(&enc, cbor_buf, sizeof(cbor_buf));
    cbor_map_open(&enc, g_update_cfg_map_hdr);
    cbor_encode_str(&enc, g_key_type);
    cbor_encode_uint8(&enc, *payload, 0);

    if (*payload == '\0') {
        /* Full update: include version and fragment info */
        if ((*g_log_ctx & 7) > 2) {
            LOG_DBG("modem_nordic_update_config type=0: version=%s fragment=%s",
                    payload + 9, payload + 0x30);
        }
        cbor_encode_str(&enc, g_key_version);
        cbor_encode_str_raw(&enc, payload + 0x30);
        cbor_encode_str(&enc, g_key_fragment);
        cbor_encode_str_raw(&enc, payload + 9);
    }

    int enc_len = cbor_map_close(&enc, cbor_buf);
    if (enc_len == 0) {
        /* Publish to SPI/MQTT topic 0xfa05 */
        int err = spi_mqtt_publish(0x81, &g_topic_0xfa05, cbor_buf,
                                   *(uint32_t *)(enc.output_buf + 4));
        if (err == 0) return;
        if ((*g_log_ctx & 7) == 0) return;
        LOG_ERR("modem_nordic_update_config: publish failed, err %d", err);
        spi_publish_error(g_spi_ctx, g_err_str, 3);
    } else {
        spi_publish_error(g_spi_ctx, g_enc_err_str, 2);
    }
}


/* ===== modem_signal_report  (OEM 0x00013b34) [confidence: medium] =====
 * Publishes current LTE modem signal quality (RSRP, RSRQ, SNR, band, PLMN, firmware version) as a CBOR map to the i.MX8 via the SPI MQTT bridge. Called periodically and on link events. */
/* OEM addr 0x13b34 – modem signal / state report publisher */
void modem_signal_report(void)
{
    /* Check modem initialised and LTE active */
    if (((*g_modem_state & 0xfb) == 1) && modem_is_connected(g_lte_ctx)) {
        LOG_INF("modem not ready for signal report");
        spi_publish_error(g_spi_ctx, g_busy_str, 2);
        return;
    }

    /* Build CBOR map with modem signal parameters */
    struct cbor_encoder enc;
    uint8_t cbor_buf[32];
    cbor_encoder_init(&enc, cbor_buf, sizeof(cbor_buf));
    cbor_map_open(&enc, g_signal_map_hdr);
    cbor_encode_uint8(&enc, g_key_state,    *g_modem_state);

    if ((*g_modem_state & 0xfb) == 1) {
        /* Include radio parameters only when registered */
        cbor_encode_uint16(&enc, g_key_rsrp,  *(uint16_t *)(g_lte_ctx + 0xe0));
        cbor_encode_uint16(&enc, g_key_rsrq,  *(uint16_t *)(g_lte_ctx + 0x1c0));
        cbor_encode_uint16(&enc, g_key_snr,   *(uint16_t *)(g_lte_ctx + 0x230));
        cbor_encode_uint8 (&enc, g_key_band,  *(uint8_t  *)(g_lte_ctx + 0x620));
        uint64_t ts = k_uptime_get();
        cbor_encode_uint64(&enc, g_key_ts,    ts);
        cbor_encode_str   (&enc, g_key_plmn,  g_lte_ctx + 0x312);
        cbor_encode_str   (&enc, g_key_fw,    g_lte_ctx + 0x5b2);
        cbor_encode_str   (&enc, g_key_iccid, g_lte_ctx + 0x542);
    }

    int enc_len = cbor_map_close(&enc, cbor_buf);
    if (enc_len == 0) {
        int err = spi_mqtt_publish(0x81, &g_topic_0xfa02, cbor_buf,
                                   *(uint32_t *)(enc.output_buf + 4));
        if (err == 0) return;
        if ((*g_log_ctx & 7) == 0) return;
        LOG_ERR("signal report publish failed, err %d", err);
        spi_publish_error(g_spi_ctx, g_err_str, 3);
    } else {
        spi_publish_error(g_spi_ctx, g_enc_err_str, 2);
    }
}


/* ===== modem_app_init  (OEM 0x00013f3c) [confidence: medium] =====
 * Top-level application init: initialises nRF modem lib, sets up TLS credentials, registers MQTT subscriptions, brings up LTE link and PPP socket pair, then starts the recurring MQTT publish tasks. */
/* OEM addr 0x13f3c – modem application init orchestrator */
void modem_app_init(void)
{
    int err;

    /* nrf_modem_lib_init / modem readiness check */
    err = nrf_modem_lib_init_check();
    if (err == 0) {
        LOG_DBG("Modem lib already initialised");
    }
    modem_uart_open(g_uart_dev);
    modem_rx_start(g_uart_dev);

    /* modem_lte_config: bring up LTE link */
    err = modem_lte_connect();
    if (err != 0) {
        LOG_ERR("LTE connect failed, err %d", err);
        modem_lte_reset(0);
        modem_net_shutdown();
        net_mgmt_enable(1);
        modem_tls_creds_init();
        return;
    }

    modem_cgerep_enable();

    /* Retry lte_lc_connect up to 3 times */
    for (int i = 3; i > 0; i--) {
        err = lte_lc_connect();
        if (err == 0) break;
        LOG_ERR("lte_lc_connect failed, err %d", err);
        k_msleep(0x8000);
    }

    /* Retry modem PSM/eDRX init up to 3 times */
    for (int i = 3; i > 0; i--) {
        err = modem_psm_init();
        if (err == 0) break;
        LOG_ERR("modem_psm_init failed, err %d", err);
        k_msleep(0x8000);
    }

    modem_proto_table_init(g_proto_ctx);
    modem_at_init(g_at_notif_ctx);
    modem_heartbeat_init();

    err = modem_socket_pair_setup(g_socket_config);
    if (err != 0) {
        LOG_ERR("socket pair setup failed, err %d", err);
    }

    /* TLS credential registration (two cert slots) */
    err = tls_credential_add(g_tls_ca_cert);
    if (err != 0) {
        LOG_ERR("CA cert add failed, err %d", err);
    }
    err = tls_credential_add(g_tls_client_cert);
    if (err != 0) {
        LOG_ERR("Client cert add failed, err %d", err);
    }

    net_mgmt_ppp_enable();
    net_mgmt_lte_enable();
    modem_tls_creds_init();

    err = modem_dns_init(g_dns_cfg);
    if (err != 0) {
        LOG_ERR("DNS init failed, err %d", err);
    }

    /* Start periodic MQTT publish tasks */
    mqtt_publish_lte_state();
    mqtt_publish_modem_state();
    mqtt_publish_status();
}


/* ===== ppp_data_forward_loop  (OEM 0x000145d4) [confidence: medium] =====
 * Data-forwarding thread body: polls the modem socket fd, reads available data with recv, and forwards it to the Zephyr PPP socket via send. Loops indefinitely; likely the 'send_data_thread' or 'recv_data_thread' entry. */
/* OEM addr 0x145d4 – PPP data forwarding thread
 * Called as a Zephyr thread entry (no direct callers in code graph).
 * Bridges modem socket <-> Zephyr PPP socket. */
void ppp_data_forward_loop(void *arg1, void *arg2, void *arg3)
{
    (void)arg1; (void)arg2; (void)arg3;

    /* g_lte_iface_idx = net_if_get_by_iface(g_lte_iface) */
    int lte_if_idx = net_if_get_by_iface(g_lte_iface);

    struct zsock_pollfd fds[1];
    fds[0].fd     = *g_modem_sock_fd;
    fds[0].events = POLLIN;

    static uint8_t buf[0x14]; /* 20-byte scratch buffer */
    memset(buf, 0, sizeof(buf));

    /* g_ppp_fwd_state = k_uptime_get(); */
    k_uptime_get_32();

    while (1) {
        /* Wait up to 1 s for data on modem socket */
        while (*g_modem_sock_fd < 0 || *g_zephyr_sock_fd < 0) {
            /* block until sockets are ready */
            k_sem_take(g_ppp_sock_ready_sem, K_FOREVER);
        }

        int nfds = 1;
        fds[0].fd = *g_modem_sock_fd;
        fds[0].events = POLLIN;
        int ret = zsock_poll(fds, nfds, 1000);
        if (ret < 1) break;

        /* Receive from modem socket */
        ret = zsock_recv(*g_modem_sock_fd, g_ppp_rx_buf, sizeof(g_ppp_rx_buf), 0);
        if (ret < 0) {
            int err = -errno;
            if (err != -EAGAIN && err != -EWOULDBLOCK) {
                LOG_WRN("%s: cannot send data from modem to PPP link"
                        " - dropped data of len %d, err %d",
                        __func__, 0, err);
            }
            continue;
        }

        if (ret > 0) {
            /* Forward to Zephyr PPP socket */
            int sent = zsock_send(*g_zephyr_sock_fd, g_ppp_rx_buf, ret, 0);
            if (sent < 0 && (sent != -EAGAIN)) {
                /* lost carrier – signal ppp_ctrl */
                LOG_DBG("PPP: Lost carrier, restarting..\n");
            }
        }
    }
}


/* ===== ppp_carrier_on  (OEM 0x0001493c) [confidence: medium] =====
 * PPP start: marks the subsystem active, drives PPP carrier-on on the Zephyr network interface, and submits the connection-listen work item so the ppp_ctrl thread starts accepting LCP negotiation. */
/* OEM addr 0x1493c – PPP start / carrier-on */
void ppp_carrier_on(void)
{
    if (*g_ppp_active_flag != '\0') {
        LOG_INF("PPP already started\n");
        return;
    }
    *g_ppp_active_flag = '\x01';

    /* net_if_carrier_on via vtable */
    net_mgmt_t *iface_mgmt = *(net_mgmt_t **)(g_ppp_iface_ctx + 8);
    if (iface_mgmt->start != NULL) {
        iface_mgmt->start(g_ppp_iface_ctx, g_ppp_iface);
    }
    /* Listen on UART: submit receive-enable work */
    net_mgmt_iface_listen(g_ppp_iface_ctx, g_ppp_rx_cfg,
                          4, K_FOREVER);

    LOG_DBG("PPP: started\n");
}


/* ===== ppp_sockets_close  (OEM 0x000149c4) [confidence: medium] =====
 * Tears down the PPP socket pair: closes the modem PPP socket and the Zephyr PPP socket, drives the PPP network interface carrier off, then triggers the nRF PPP context down sequence. */
/* OEM addr 0x149c4 – PPP socket teardown ("PPP: stopped" path) */
void ppp_sockets_close(void)
{
    if (*g_ppp_active_flag == '\0') {
        LOG_INF("PPP already stopped\n");
        return;
    }
    *g_ppp_active_flag = '\0';

    /* Get LTE interface context */
    int lte_ctx = modem_iface_find(g_iface_db);
    if (lte_ctx == 0) {
        __ASSERT(0, "PPP context not found.\n");
    }
    modem_rx_disable(*(uint32_t *)(lte_ctx + 0x21c));
    k_work_submit(g_ppp_stop_work);

    /* Close modem PPP socket */
    int modem_fd = *g_modem_sock_fd;
    if (modem_fd != -1) {
        *g_modem_sock_fd = -1;
        LOG_DBG("Closing PPP modem socket");
        int err = zsock_close(modem_fd);
        if (err != 0) {
            LOG_WRN("Closing of PPP modem socket failed, errno %d", errno);
        }
    }
    k_msleep(0x10000);

    /* Close Zephyr PPP socket */
    int zephyr_fd = *g_zephyr_sock_fd;
    if (zephyr_fd != -1) {
        *g_zephyr_sock_fd = -1;
        LOG_DBG("Closing PPP zephyr socket");
        int err = zsock_close(zephyr_fd);
        if (err != 0) {
            LOG_WRN("Closing of PPP zephyr socket failed, errno %d", errno);
        }
    }

    /* Drive PPP carrier off on the network interface */
    ppp_iface_carrier_off();
    net_if_carrier_off(g_ppp_iface);

    LOG_INF("PPP: stopped");
}


/* ===== ppp_socket_setup  (OEM 0x00014b48) [confidence: medium] =====
 * Creates and configures the PPP modem socket and the Zephyr PPP network socket, applies SO_SNDTIMEO / SO_RCVTIMEO to each, and binds the Zephyr side. On success marks the PPP interface ready. */
/* OEM addr 0x14b48 – PPP socket pair creation */
void ppp_socket_setup(void)
{
    int err;
    struct timeval tv;

    /* Get LTE interface MTU and cap PPP MTU */
    err = modem_lte_interface_get(g_lte_iface);
    if (err == 0) {
        uint16_t mtu = lte_if_get_mtu(g_lte_iface);
        if (mtu > 1500) {
            LOG_WRN("LTE link MTU (%d) cannot be set as PPP MTU. Setting to the max: %d\n",
                    mtu, 1500);
            mtu = 1500;
        }
        *g_ppp_mtu = mtu;
        if (g_lte_iface != NULL) {
            *(uint16_t *)(g_lte_iface->data + 0x18) = mtu;
        }
    }

    /* Zephyr PPP data socket: AF_PACKET / ETH_P_PPP */
    if (*g_zephyr_sock_fd == 0xffffffff) {
        int sock = zsock_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_PPP));
        *g_zephyr_sock_fd = sock;
        if (sock < 0) {
            LOG_ERR("PPP Zephyr data socket creation failed: (%d)\n",
                    errno);
            LOG_ERR("Cannot create zephyr socket for ppp: %d", errno);
        } else {
            LOG_DBG("PPP Zephyr data socket %d created", sock);
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
            err = zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (err < 0) {
                LOG_WRN("Unable to set socket SO_SNDTIMEO - continue");
            }
            err = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (err < 0) {
                LOG_WRN("Unable to set socket SO_RCVTIMEO - continue");
            }
            struct sockaddr_ll addr = {0};
            addr.sll_family  = AF_PACKET;
            addr.sll_ifindex = net_if_get_by_iface(g_ppp_iface);
            err = zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
            if (err < 0) {
                LOG_WRN("Failed to bind PPP data socket : %d", errno);
            }
        }
        k_mutex_unlock(g_zephyr_sock_mutex);
        k_mutex_unlock(g_zephyr_sock_mutex);
        ppp_iface_carrier_on();
        return;
    } else {
        LOG_INF("PPP Zephyr data socket already up - nothing to do");
    }

    /* Modem PPP socket: AF_INET / SOCK_RAW / IPPROTO_PPP */
    if (*g_modem_sock_fd == 0xffffffff) {
        int sock = zsock_socket(AF_INET, SOCK_RAW, 0xff);
        *g_modem_sock_fd = sock;
        if (sock < 0) {
            LOG_ERR("Cannot create modem socket for ppp: %d", errno);
            LOG_ERR("Cannot create modem socket for ppp: %d", sock);
        } else {
            LOG_DBG("modem data socket %d created for modem data", sock);
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
            err = zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (err < 0) {
                LOG_WRN("Unable to set socket SO_SNDTIMEO - continue");
            }
            err = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (err < 0) {
                LOG_WRN("Unable to set socket SO_RCVTIMEO - continue");
            }
            struct sockaddr_ll addr = {0};
            net_if_get_by_iface(g_lte_iface);
            err = zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
            if (err >= 0) {
                goto done;
            }
            if (err < 0) {
                zsock_close(sock);
            }
        }
        LOG_ERR("Cannot create modem socket for ppp: %d", -1);
    } else {
        LOG_INF("PPP modem socket already up - nothing to do");
    }
done:
    ppp_teardown();
    return;
}
