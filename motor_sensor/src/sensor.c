/*
 * sensor.c — VanMoof S5 motor_sensor: magnetic rotor-angle sensor IC access.
 *
 * The sensor IC is reached through a vtable-dispatched driver object (global @
 * 0x1301FE00): a readiness probe + a register-block read/write, in either a
 * direct (classical-CAN) or an FD path. sensor_speed_sample is the top-level
 * speed reader consumed by the motor_sensor task. Translated from the OEM image
 * (LPC546xx-class, base 0x0); the driver primitives are vendor.
 *
 * OEM: sensor_reg_block_write 0x3170, sensor_config_write 0x3204,
 * sensor_position_read 0x3284, sensor_speed_sample 0x3310.
 */

#include "motor_sensor.h"

/* RAM globals reached via literal pools. */
extern sensor_obj_t *g_sensor_obj;      /* DAT_3200/327c/3308 = 0x1301FE00 */
extern int (*g_direct_write_fn)(void *, uint32_t, uint32_t, void *, uint32_t, uint32_t); /* 0x1300427D */
extern uint32_t g_sensor_diag_ch;       /* 0xE6384427 */
extern void   **g_sensor_handle_ptr;    /* 0x200000F8 -> SensorHandle* */
extern uint32_t g_diag_module;          /* 0xC02094D8 */
extern const uint8_t g_ref_pattern[6];  /* 0x00006D63 — 6-byte frame-header reference */

/* ------------------------------------------------------------------- 0x3170 */

/* Gate (TX idle + RX drained) then write a 512-byte register block to the
 * sensor at reg_addr, via the direct fn ptr (classical) or vtable[+0x14] (FD).
 * Logs diag 0x31/0x38/0x42 on step failure; returns 0 / -1. */
int sensor_reg_block_write(void *handle, uint32_t reg_addr, void *buf)
{
    uint32_t out_a, out_b;

    if (can_tx_poke(NULL, 0) != 0) {                 /* TX must be idle */
        can_diag_log_send(g_sensor_diag_ch, 0x31, 0);
        return -1;
    }
    if (can_rx_dequeue() != 0) {                      /* drain RX */
        can_diag_log_send(g_sensor_diag_ch, 0x38, 0);
        return -1;
    }

    if (can_is_fd_mode(g_sensor_obj)) {
        /* FD path: vtable[+0x14] fd-write */
        ms_fdwrite_fn fd_write = (ms_fdwrite_fn)g_sensor_obj->vtable[0x14 / 4];
        if (fd_write(handle, reg_addr, 0x200, buf, NULL, &out_b, &out_a) != 0) {
            can_diag_log_send(g_sensor_diag_ch, 0x42, 0);
            return -1;
        }
    } else {
        /* classical path: direct RAM fn ptr */
        if (g_direct_write_fn(handle, reg_addr, 0x200, &out_b, (uint32_t)(uintptr_t)&out_a, 0) != 0) {
            can_diag_log_send(g_sensor_diag_ch, 0x42, 0);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- 0x3204 */

/* Config-write sequencer: probe readiness at 0xfa00; if not ready return -3
 * (the "not-yet-configured" sentinel callers key off). Otherwise dispatch the
 * config, write the 0xf400 register block, and finalise. Returns 0 / -3. */
int sensor_config_write(void *handle, void *buf, void *p3)
{
    ms_probe_fn probe = (ms_probe_fn)g_sensor_obj->vtable[0x10 / 4];
    int rc = 0;
    (void)p3;

    if (probe(handle, SENSOR_CFG_REG, 0x200, (vfn_t)probe) == 0)
        return -3;                                   /* not yet configured */

    if (can_tx_dispatch(handle, SENSOR_CFG_REG, 0x200, NULL) != 0) {
        can_diag_log_send(g_sensor_diag_ch, 0x65, 0);
        return rc;
    }
    rc = sensor_reg_block_write(handle, SENSOR_DATA_REG, buf);
    if (rc == 0 && can_tx_poke(handle, SENSOR_CFG_REG) != 0) {
        can_diag_log_send(g_sensor_diag_ch, 0x73, 0);
        /* fall through — still return rc(0) */
    }
    return rc;
}

/* ------------------------------------------------------------------- 0x3284 */

/* Read the 12-byte position frame. If the sensor is already configured
 * (config_write==0) the bytes come from the config read buffer; if not
 * (config_write==-3) fall back to a direct probe+read at 0xf400. */
int sensor_position_read(void *handle, uint8_t out_buf[12])
{
    uint8_t  stack_buf[512];
    uint32_t rd = 0;
    int      rc;

    if (handle == NULL)
        return -2;

    rc = sensor_config_write(handle, stack_buf, NULL);
    if (rc == -3) {
        /* not configured: direct vtable probe + dispatch-read at 0xf400 */
        ms_probe3_fn probe = (ms_probe3_fn)g_sensor_obj->vtable[0x10 / 4];
        if (probe(handle, SENSOR_DATA_REG, 0x200) == 0) {
            mem_set(out_buf, 0, 0xc);                 /* no data */
            return 0;
        }
        if (can_tx_dispatch(handle, SENSOR_DATA_REG, 0xc, out_buf) != 0) {
            can_diag_log_send(g_sensor_diag_ch, 0xa1, 3, SENSOR_DATA_REG, rd);
            return -1;
        }
        return 0;
    }
    if (rc != 0)
        return rc;
    mem_cpy(out_buf, stack_buf, 0xc);                 /* cached config data */
    return 0;
}

/* ------------------------------------------------------------------- 0x3310 */

/* Top-level speed sampler: read the 12-byte position frame, validate its first
 * 6 bytes against the RAM reference pattern, and return the 32-bit speed/count
 * at frame offset +8 on success (0 on null handle or header mismatch). */
uint32_t sensor_speed_sample(void)
{
    void   *handle = *g_sensor_handle_ptr;
    uint8_t frame[12];

    if (handle == NULL) {
        can_diag_log_send(g_diag_module, 0x25, 0);
        return 0;
    }
    if (sensor_position_read(handle, frame) != 0)
        return 0;
    if (strncmp_(frame, g_ref_pattern, 6) != 0) {     /* header mismatch */
        can_diag_log_send(g_diag_module, 0x2e, 5, frame[0], frame[1], frame[2], frame[3], frame[4]);
        return 0;
    }
    return *(uint32_t *)&frame[8];                     /* raw rotor speed/count */
}
