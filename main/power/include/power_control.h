/*
 * power_control.h — battery/charger command set + power-control OD binding
 *
 * OEM: /usr/bin/power, devices/main/power/src/power_control.cpp.
 *
 * The PowerControl object is embedded in PowerService at offset 0x50
 * (PS_power_control). It owns the CAN/OD command channel to the power-control
 * board (node a0=0xA3) and subscribes to the power_control_* / battery_* OD
 * signals. From it the daemon issues the battery/charger command set and
 * tracks primary-battery insertion.
 *
 * This header models the subobject by its OEM field offsets so the .c can do
 * MMIO-style field access matching the binary.
 */
#ifndef POWER_CONTROL_H
#define POWER_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "power_common.h"

/*
 * PowerControl subobject (PS_power_control, PowerService+0x50). Only the fields
 * touched by power_control.cpp are named; offsets are relative to the subobject
 * base. Verified against the disassembly of FUN_00133820 / battery_send_command.
 */
typedef struct PowerControl PowerControl;
struct PowerControl { uint8_t _raw[0x60]; };

#define PC_FIELD(p, off, ty)  (*(ty *)((uint8_t *)(p) + (off)))

#define PC_cmd_channel(p) PC_FIELD(p, 0x08, void *)    /* OD entry: power-control cmd (key 0x1a3) */
#define PC_cmd_arg(p)     PC_FIELD(p, 0x40, void *)    /* publisher arg for the command send      */
#define PC_insdet_fail(p) PC_FIELD(p, 0x48, uint8_t)   /* frame[2]>>2 & 1 : BatteryINS-DET FAIL   */
#define PC_inserted(p)    PC_FIELD(p, 0x49, uint8_t)   /* frame[2] & 1   : IsPrimaryInserted      */
#define PC_last_state(p)  PC_FIELD(p, 0x80, uint8_t)   /* last power_control_state byte (frame[0])*/
#define PC_have_status(p) PC_FIELD(p, 0x81, uint8_t)   /* a status frame has been received        */
#define PC_ipc(p)         PC_FIELD(p, 0x88, void *)    /* IMQTTClient / OD publisher              */
#define PC_switch(p)      PC_FIELD(p, 0x90, void *)    /* switch_control (send / connected query) */
#define PC_initialized(p) PC_FIELD(p, 0x10, uint8_t)   /* power-control init complete             */

/* vtable on the PowerControl object itself (*(p)+0x10..+0x28): per-state hooks. */

/* ---- the battery/charger command set ----------------------------------- */

/* OEM 0x138f90: open the power-control command OD entry, write `opcode`,
 * commit. `cmd_channel` is the OD handle (PC_cmd_channel). */
void battery_stamp_command(void *cmd_channel, const uint8_t *opcode);

/* OEM 0x133590: stamp `*opcode` into the command channel and send the frame to
 * the power-control board via the switch/IPC publisher. */
void power_control_send_command(PowerControl *pc, const uint8_t *opcode);

/* OEM 0x133730 / 0x1335d0 / 0x133780 / 0x1337d0: thin opcode wrappers. */
void battery_cmd0_off(PowerControl *pc);
void battery_cmd1_on(PowerControl *pc);
void battery_cmd8_shipping(PowerControl *pc);
void battery_cmd9_clear_fault(PowerControl *pc);

/* OEM 0x133660: clear charger test/burn-in, then send IdentifyCharger (cmd 5). */
void charger_cmd5_identify(PowerControl *pc);

/* OEM 0x133ac0: set IsPrimaryInserted override true, then send reset (cmd 6). */
void battery_cmd6_reset(PowerControl *pc);

/* OEM 0x133620: two raw `cansend` calls that clear the charger's test &
 * burn-in mode (writes 0xA55A00 to charger OD ids 0x14E23214 / 0x14E23210). */
void power_control_clear_charger_test_burnin(PowerControl *pc);

/* OEM 0x133450: read the cached primary-insertion flag (PC_inserted). */
bool get_is_primary_inserted(const PowerControl *pc);

/* OEM 0x133820: OD callback for `power_control_state`. Echoes the last command,
 * decodes insertion/state from the status frame, and dispatches the per-state
 * vtable hooks. `f` is the raw 8-byte status frame. */
void power_control_on_state(PowerControl *pc, void *unused, const uint8_t *f);

#endif /* POWER_CONTROL_H */
