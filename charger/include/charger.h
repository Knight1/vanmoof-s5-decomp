#ifndef CHARGER_H
#define CHARGER_H

/*
 * charger.h — shared declarations for the VanMoof S5 Liteon charger application
 * layer (model 5EL00000000EB).
 *
 * Reconstructed from the OEM image
 *   charger_liteon_normal.0.0.1.8.0.untagged.x.bin
 * (raw ARM Cortex-M, image base 0x0, initial SP 0x20008000, soft-float). Two
 * build variants ship — `normal` (v0.0.1.8.0) and `speed` (v0.0.1.2.0, fast
 * charge) — which are the SAME code differing only in a charge-control parameter
 * (0x69 -> 0x09) and version (see docs/variants.md).
 *
 * Only the charger APPLICATION layer is reconstructed; newlib/libc, the Bosch
 * M_CAN driver + register HAL, the cooperative mini-RTOS/event core and flash
 * self-programming are vendor. RAM globals are named `#define`s accessed as
 * literal-address volatile casts so the emitted code matches the OEM.
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* ------------------------------------------------------- device globals --- */
#define CHG_SETPOINT_CTX   0x20000a94u  /* charge-setpoint output struct (HW DAC/PWM fields) */
#define CHG_STATUS_WORD    0x20000aacu  /* u16 software status / fault bit-field */
#define CHG_FAULT_STATE    0x20000ac2u  /* charger fault-state RAM global (u16; cleared via strh) */
#define CHG_MODE_WORD      0x20000ab6u  /* u16 charge mode/state word */
#define CHG_COMPLETE_FLAG  0x20000b03u  /* u8 charge-complete flag */
#define CHG_STATUS_BYTE    0x20000b04u  /* u8 charge status flags */
#define CHG_STATE_BYTE     0x20000b05u  /* u8 charge state (0/1/2/3/5) */
#define CHG_SETPOINT_RAW   0x20000a92u  /* u16 intermediate per-state setpoint value (= CHG_SETPOINT_CTX-2; OEM literal @0x2be8) */
#define CHG_TELEM          0x200008c8u  /* telemetry struct: u8 base-limit @0, u16 voltage @+2 */
#define CHG_TELEM_SCALE    0x2000004au  /* u8 voltage-derate scale */
#define CHG_TEMP_STRUCT    0x20000898u  /* temperature struct: s16 temperature @+4 */
#define CHG_LIMIT_A        0x200008d4u  /* u8 base charge-current limit (variant A) */
#define CHG_TEMP_TABLE     0x00007e64u  /* 11-entry temperature breakpoint table (flash, off-image) */
#define CHG_PROFILE_THRESH_HI 999999u   /* 0x000f423f */
#define CHG_PROFILE_THRESH_LO 799999u   /* 0x000c34ff */
#define CHG_FAULT_PERIPH   0x40001000u  /* peripheral base; fault control reg @+0x58 */
#define CHG_FAULT_TABLE    0x000080b0u  /* 8-entry fault-code table (flash, off-image) */
#define CHG_VERSION_ID     0x110eu      /* device/version id returned by get_version_id (4366) */
#define CHG_IDENT_REC1     0x20000024u  /* identity record 1 buffer */
#define CHG_IDENT_REC2     0x20000038u  /* identity record 2 buffer (= REC1 + 0x14) */
#define CHG_FLASH_CTX      0x200008e4u  /* flash driver context */
#define CHG_FLASH_KEY      0x6b65666cu  /* flash program key */
/* charger telemetry report CAN IDs (charger OD node, 0x14E2xxxx) + packing consts */
#define CHG_RPT_ID_1       0x14e25460u  /* 8B: current setpoint + voltage + limits */
#define CHG_RPT_ID_2       0x14e2b462u  /* 2B */
#define CHG_RPT_ID_3       0x14e25461u  /* 1B: charge mode */
#define CHG_RPT_ID_4       0x14e23460u  /* 6B: packed status nibbles */
#define CHG_RPT_CUR_SCALE  480000u      /* 0x00075300 current-setpoint scale */
#define CHG_RPT_LIMITS     0x07d0d930u  /* packed setpoint limits (0xd930 / 0x07d0) */
#define CHG_IDENT_FLAG     0x20000d08u  /* u8 identity-written flag */
#define CHG_CRC_CTX_PTR    0x20000000u  /* holds ptr to the CRC context (result at +8) */
#define CHG_IDENT_RESULT   0x200000a8u  /* u32 last identity CRC result */
/* CAN-register event dispatch tables (shared by the 4 dispatchers). */
#define CHG_DISP_KEYS      0x000080b0u  /* off-image: lookup keys = arg0 (== fault table) */
#define CHG_DISP_ARG1      0x20000978u  /* RAM arg1 table */
#define CHG_DISP_HANDLERS  0x2000099cu  /* RAM handler function-pointer table */

/* temperature breakpoint table entry (12 bytes); lives in flash beyond the
 * loaded image (CHG_TEMP_TABLE = 0x7e64), satisfied at link by the real flash. */
typedef struct __attribute__((packed)) {
    int16_t thresh;   /* +0  temperature threshold */
    int16_t _pad;     /* +2 */
    int32_t slope;    /* +4 */
    int32_t offset;   /* +8 */
} charge_temp_bp_t;

/* charge-config init constant (DAT_000007d8): nominal full-scale, 1e6 (uA / count) */
#define CHG_CFG_FULLSCALE  1000000u     /* 0x000f4240 */
#define CHG_CFG_SETPOINT   0x0a03u      /* default charge setpoint word (2563) */

/* ------------------------------------------------- vendor callees (deferred) */
extern void     mem_set(void *dst, int c, uint32_t n);   /* vendor libc // thunk_FUN_000057dc (memset) */
extern uint32_t charge_read_status_bits(void);           /* vendor hal // 0x00002c5c (CAN/status reg read) */
extern int      charge_read_state(void);                 /* vendor hal // 0x00003000 (state accessor) */
extern int      charge_read_value16(void);               /* vendor hal // 0x00003018 (u16 accessor) */
extern void     charge_teardown_step(void);              /* vendor // 0x00002bf0 */
extern void     charge_post_mode(int code);              /* vendor // 0x00002b70 */
extern void     charge_clear_output(int x);              /* vendor // 0x00002c9c */
extern void     charge_notify_state(void);               /* vendor // 0x00002b58 */
extern void     charge_event_post(int id, int code, int arg); /* vendor rtos // 0x00001074 (event/message post) */
extern void     charge_status_cmd(int cmd);              /* vendor hal // 0x00002c48 */
extern int      charge_read_arm_flag(void);              /* vendor hal // 0x00002fe8 */
extern void     charge_arm_tx(int param);                /* vendor hal // 0x00001d38 (M_CAN Tx arm; carries variant param) */
extern void     charge_helper_1320(void);                /* vendor // 0x00001320 */
extern void     charge_helper_16b0(void);                /* vendor // 0x000016b0 */
extern void     charge_helper_1cdc(void);                /* vendor // 0x00001cdc */
extern void     charge_helper_1cf0(void);                /* vendor // 0x00001cf0 */
extern void     charge_helper_1d18(void);                /* vendor // 0x00001d18 */
extern int      charge_flash_erase(void *ctx, uint32_t addr, uint32_t len, uint32_t key); /* vendor hal // 0x000006b4 */
extern void     charge_flash_write(void *ctx, uint32_t addr, void *buf, uint32_t len);    /* vendor hal // 0x000006e4 */
extern void     charge_cmd_xfer_setup(void);             /* vendor // 0x000043a8 */
extern void     charge_cmd_send(void *p);                /* vendor // 0x0000580e */
extern void     charge_cmd_recv(void *p);                /* vendor // 0x0000534e */
extern void     charge_cmd_copy(void *dst, void *src, uint32_t n); /* vendor // 0x0000584a */
extern void     charge_cmd_finish(void *buf);            /* vendor // 0x0000547c */
extern int      charge_query_chg(int a, int b);          /* vendor hal // 0x00004f68 (charge-present poll) */
extern uint32_t charge_read_nibble_a(void);              /* vendor hal // 0x00002a48 */
extern uint32_t charge_read_nibble_b(void);              /* vendor hal // 0x00002c90 */
extern void     charge_send_report(int kind);            /* vendor // 0x0000261c (Tx report) */
extern int      charge_query_1c54(void);                 /* vendor // 0x00001c54 */
extern void     charge_helper_1c48(int x);               /* vendor // 0x00001c48 */
extern int      charge_read_meas(void);                  /* vendor hal // 0x00002a84 (measured value) */
extern int      charge_query_2ca8(void);                 /* vendor // 0x00002ca8 */
extern int      charge_read_mode(void);                  /* vendor hal // 0x00002b64 */
extern int      charge_get_limit(void);                  /* vendor // 0x0000506a (returns 0x12a7) */
extern void     charge_tx_enqueue(uint32_t id, int len, void *buf); /* vendor hal // 0x00001474 (M_CAN Tx) */
extern int      charge_read_19e0(void);                  /* vendor hal // 0x000019e0 (voltage measure) */
extern uint32_t charge_read_1a74(void);                  /* vendor hal // 0x00001a74 */
extern uint32_t charge_read_3394(void);                  /* vendor hal // 0x00003394 (status byte) */
extern int      charge_flash_op(uint32_t addr, uint32_t mode);    /* vendor hal // 0x00001e64 (flash erase/prep) */
extern void     charge_error_post(int code);             /* vendor // 0x000037d0 (error/event) */
extern uint32_t charge_crc(uint32_t addr, uint32_t len); /* vendor // 0x0000122c (one-shot CRC) */
extern void     charge_flash_prep(uint32_t addr);        /* vendor hal // 0x00001db4 */
extern void     charge_flash_write2(uint32_t addr, void *buf);    /* vendor hal // 0x00001df8 */
extern void     charge_crc_init(void *ctx);              /* vendor // 0x0000117c */
extern void     charge_crc_update(void *ctx, void *data, uint32_t len); /* vendor // 0x00004a16 */

/* --------------------------------------------------------- VanMoof prototypes */
/* charge.c */
void charger_charge_state_init(void *cfg);                    /* 0x000007b0 */
int  charger_set_charge_setpoint(int raw);                    /* 0x00002a54 */
int  charger_set_charge_scaled_setpoint(uint32_t raw);        /* 0x00002a90 */
void charger_status_flag_set(uint16_t mask);                  /* 0x00002c68 */
void charger_status_flag_clear(uint16_t mask);               /* 0x00002c7c */
void charger_clear_fault_state(void);                         /* 0x00003a84 */

/* state.c */
void charger_set_charge_status_flag(int cmd);                 /* 0x00003270 */
void charger_charge_state5_to_state2(void);                   /* 0x00003724 */
void charger_set_charge_mode(uint16_t mode);                  /* 0x00003758 */
void charger_select_charge_setpoint_by_state(void);           /* 0x00002b7c */
void charger_charge_ctx_init(void *ctx);                      /* 0x00004f04 */
void charger_ctx_init(void *ctx);                             /* 0x00004b9c */
void charger_can_msg_init(void *msg);                         /* 0x00004c32 */
uint32_t charger_charge_enable_set(void *dev, int mode,
                                   void *state_ctx, const uint32_t *setpoint); /* 0x00004e76 */

/* fsm.c */
void charger_charge_state_dispatch(int new_state);            /* 0x00003150 */
void charger_charge_event_dispatch(int code);                 /* 0x00002ac0 */
void charger_charge_step_finalize(void);                      /* 0x00005078 */
void charger_charge_state_machine_step(void);                 /* 0x000033a0 */

/* report.c */
void charger_build_can_reports(uint32_t arg0, uint32_t arg1); /* 0x000023e4 */

/* identity.c */
void charger_write_identity_block(int mode);                  /* 0x000039dc */
int  charger_read_identity_version(int mode);                 /* 0x00005166 */

/* dispatch.c — CAN-register event dispatchers */
void charger_dispatch_can_86000(void);                        /* 0x00003ea4 */
void charger_dispatch_can_89000(void);                        /* 0x00003f34 */
void charger_dispatch_can_8a000(void);                        /* 0x00003f64 */
void charger_dispatch_can_9f000(void);                        /* 0x00004024 */

/* misc.c */
void charger_report_fault_event(void);                        /* 0x000016c0 */
int  charger_find_fault_index(int code);                      /* 0x00003e88 */
int  charger_get_version_id(void);                            /* 0x00005070 */
int  charger_write_identity_config_record(void);              /* 0x00003024 */
int  charger_process_command_record(void *rec);               /* 0x00005424 */

/* profile.c */
uint32_t charger_compute_charge_current_limit(void);          /* 0x000019b0 */
uint32_t charger_compute_charge_current_setpoint(void);       /* 0x000019d4 */
int      charger_compute_charge_profile_setpoints(uint32_t base, uint32_t total, void *frame); /* 0x00000814 */

#endif /* CHARGER_H */
