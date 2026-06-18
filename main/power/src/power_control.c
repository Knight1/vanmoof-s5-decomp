/*
 * power_control.c — battery/charger command set + power-control OD binding
 *
 * OEM: /usr/bin/power, devices/main/power/src/power_control.cpp (AArch64, C++,
 * stripped). Reconstructed clean-room from the decompiled image. The PowerControl
 * object is embedded in PowerService at PS_power_control (PowerService+0x50);
 * here it is `PowerControl *pc`.
 *
 * Responsibilities:
 *   - The battery/charger command set. Each command is one opcode byte stamped
 *     into the power-control board's command OD entry (node a0=0xA3, OD entry
 *     key 0x1a3 / id 0x14603040, opcode at frame[0]) and sent.
 *   - OnPowerControlState: the OD callback for `power_control_state`. Decodes
 *     primary-battery insertion (frame[2]&1) and the reported power-control
 *     state (frame[0]), publishes IsPrimaryInserted, and dispatches per-state
 *     hooks.
 *   - Clearing the charger's test & burn-in mode via raw `cansend`.
 *
 * The OD signal subscriptions (power_control_state/control/measurements and the
 * two *_init signals) are bound in the PowerControl constructor (OEM 0x133c20),
 * which is dominated by STL std::vector / std::function plumbing and is modelled
 * here rather than rebuilt byte-for-byte.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>     /* system() */
#include "power_common.h"
#include "power_control.h"

#define PC_SRC "devices/main/power/src/power_control.cpp"

/* ------------------------------------------------------------------------ *
 * Command channel
 * ------------------------------------------------------------------------ */

/*
 * OEM 0x138f90: battery_stamp_command
 *
 * Look up the power-control command OD entry (vm key 0x1a3 == 419, used by
 * 0x138ec0/0x138f20 to get/commit the writable buffer), write the single
 * opcode byte into it, and commit. The opcode lands at frame[0] of the CAN
 * payload sent to the power-control board.
 *
 * In the image this goes through two vm helpers:
 *   0x138ec0 vm_od_begin_write(handle, &buf): map key 0x1a3 -> writable buffer
 *   0x138f20 vm_od_commit(handle):            publish the staged buffer
 * Both return 0 on success; a non-zero return is propagated to __stack_chk_fail
 * style fast-fail. Modelled here against the od_table helpers.
 */
void battery_stamp_command(void *cmd_channel, const uint8_t *opcode)
{
    uint8_t *buf = NULL;

    /* 0x138ec0: open the writable OD buffer for key 0x1a3 (power-control cmd). */
    if (od_begin_write(cmd_channel, &buf) != 0)
        return;

    buf[0] = *opcode;          /* frame[0] = opcode */

    /* 0x138f20: commit/publish the staged frame to node a0=0xA3. */
    (void)od_commit(cmd_channel);
}

/*
 * OEM 0x133590: power_control_send_command  (a.k.a. PowerControl::sendCommand)
 *
 * Stamp the opcode into the command channel, then hand the prepared frame to
 * the switch/IPC publisher object (PC_switch, vtable slot +0x20) passing the
 * publisher arg PC_cmd_arg. The image tail-calls the vtable slot.
 *
 *   x0 = pc
 *   ldr x0,[pc+0x08]   ; cmd_channel
 *   bl  battery_stamp_command(cmd_channel, opcode)   ; opcode still in x1
 *   ldr x0,[pc+0x90]   ; switch_control object
 *   ldr x1,[pc+0x40]   ; publisher arg
 *   ldr x2,[[x0]+0x20] ; vtable->send
 *   br  x2
 */
void power_control_send_command(PowerControl *pc, const uint8_t *opcode)
{
    void  *send_obj = PC_switch(pc);
    void  *arg      = PC_cmd_arg(pc);
    void (**vtbl)(void *, void *) = *(void (***)(void *, void *))send_obj;

    battery_stamp_command(PC_cmd_channel(pc), opcode);

    /* vtable+0x20: tail-send the stamped frame. */
    vtbl[0x20 / sizeof(void *)](send_obj, arg);
}

/* ------------------------------------------------------------------------ *
 * The command set — thin opcode wrappers (OEM 0x133730 ... 0x1337d0)
 * ------------------------------------------------------------------------ */

/* OEM 0x133730: battery off (opcode 0). */
void battery_cmd0_off(PowerControl *pc)
{
    uint8_t op = BATT_CMD_OFF;
    power_control_send_command(pc, &op);
}

/* OEM 0x1335d0: battery on (opcode 1). */
void battery_cmd1_on(PowerControl *pc)
{
    uint8_t op = BATT_CMD_ON;
    power_control_send_command(pc, &op);
}

/* OEM 0x133780: enter shipping mode (opcode 8). */
void battery_cmd8_shipping(PowerControl *pc)
{
    uint8_t op = BATT_CMD_SHIPPING;
    power_control_send_command(pc, &op);
}

/* OEM 0x1337d0: clear battery fault (opcode 9). */
void battery_cmd9_clear_fault(PowerControl *pc)
{
    uint8_t op = BATT_CMD_CLEAR_FAULT;
    power_control_send_command(pc, &op);
}

/*
 * OEM 0x133620: power_control_clear_charger_test_burnin
 *
 * Two literal `cansend` invocations on vcan0 that write magic 0xA55A00 to the
 * charger's two test/burn-in OD ids. 0x14E23214 / 0x14E23210 decode (vm_address
 * a0=0xA7 charger, a1=0x11) to the charger control plane; payload A5 5A 00 is
 * the "clear" magic. Issued via system() in the image (FUN_001099e0 == system).
 */
void power_control_clear_charger_test_burnin(PowerControl *pc)
{
    (void)pc;
    common_logf(PC_SRC, 0xca, LOG_INFO,
                "Clearing test & burn-in mode of charger");
    system("cansend vcan0 14E23214#A55A00");
    system("cansend vcan0 14E23210#A55A00");
}

/*
 * OEM 0x133660: charger_cmd5_identify
 *
 * Log, clear the charger's test/burn-in mode first (so identify is not blocked),
 * then send the IdentifyCharger command (opcode 5).
 */
void charger_cmd5_identify(PowerControl *pc)
{
    uint8_t op = CHARGER_CMD_IDENTIFY;
    common_logf(PC_SRC, 0xd4, LOG_INFO, "Sending IdentifyCharger command");
    power_control_clear_charger_test_burnin(pc);
    power_control_send_command(pc, &op);
}

/*
 * OEM 0x133ac0: battery_cmd6_reset
 *
 * Log, then force the IsPrimaryInserted override true in the od-overrides map
 * (so the reset does not race a transient "not inserted"), then send the reset
 * command (opcode 6).
 *
 * In the image the override is a std::map<string,bool> keyed on the
 * "IsPrimaryInserted" signal name: FUN_0013df10 = map.contains(key);
 * FUN_0013eaa0 = map.set(key,true). The map machinery is STL/vm framework and
 * is modelled here as od_override_*(); the salient behaviour is the opcode-6
 * send.
 */
void battery_cmd6_reset(PowerControl *pc)
{
    uint8_t op = BATT_CMD_RESET;
    common_logf(PC_SRC, 0xf0, LOG_INFO, "Sending battery reset command");

    /* 0x13df10/0x13eaa0: if the IsPrimaryInserted override is not already set,
     * set it true. (od-overrides map, key = the IsPrimaryInserted OD signal.) */
    if (!od_override_contains("IsPrimaryInserted"))
        od_override_set("IsPrimaryInserted", true);

    power_control_send_command(pc, &op);
}

/* ------------------------------------------------------------------------ *
 * Insertion state
 * ------------------------------------------------------------------------ */

/* OEM 0x133450: cached primary-insertion flag (written by the status callback). */
bool get_is_primary_inserted(const PowerControl *pc)
{
    return PC_inserted(pc) != 0;
}

/* ------------------------------------------------------------------------ *
 * OD callback: power_control_state  (OEM 0x133820)
 * ------------------------------------------------------------------------ */

/*
 * OEM 0x133820: power_control_on_state  (PowerControl::onState)
 *
 * Subscribed to the `power_control_state` OD signal (bound in the ctor via the
 * 0x1359e0 std::function thunk). `f` is the raw 8-byte status frame from the
 * power-control board:
 *
 *   f[0] : reported power-control state
 *          0 -> off, 1 -> on, 3 -> standby/operational, 5 -> identify
 *   f[2] : status bits
 *          bit0 : IsPrimaryInserted (1 = primary battery present)
 *          bit2 : BatteryINS-DET FAIL
 *
 * Behaviour, verified against the disassembly:
 *   1. Echo the last stamped command: publish PC_cmd_arg via the IPC vtable+0x20.
 *   2. If the state is transitioning *into* 3 (and we either have no prior
 *      status or the last state was not already 3), clear charger test/burn-in.
 *   3. If the switch object reports "not connected" (vtable+0x28 == 0) and the
 *      state actually changed, notify it (vtable+0x10) of the new state.
 *   4. Publish IsPrimaryInserted via the override publisher with the inverted
 *      sense the image computes ((f[2]&1)^1); warn when the primary is missing.
 *   5. Cache: PC_last_state=f[0], PC_have_status=1,
 *      PC_insdet_fail=(f[2]>>2)&1, PC_inserted=f[2]&1   (atomic stlrb stores).
 *   6. If power-control is initialized, dispatch the per-state vtable hook on
 *      `pc` itself: f[0]==0 ->+0x10, ==1 ->+0x18, ==3 ->+0x20, ==5 ->+0x28;
 *      otherwise warn "Not supported yet". If not initialized, info-log
 *      "power control not initialized yet, ignoring callbacks".
 */
void power_control_on_state(PowerControl *pc, void *unused, const uint8_t *f)
{
    (void)unused;
    void  *ipc   = PC_ipc(pc);
    void  *swobj = PC_switch(pc);
    void (***ipc_vt)(void) = (void (***)(void))ipc;
    void (***sw_vt)(void)  = (void (***)(void))swobj;

    /* 1. echo last stamped command back out (IPC vtable+0x20, PC_cmd_arg). */
    ((void (*)(void *, void *))(*ipc_vt)[0x20 / sizeof(void *)])(ipc, PC_cmd_arg(pc));

    /* 2. clear charger test/burn-in when entering state 3 fresh. */
    if ((!PC_have_status(pc) || PC_last_state(pc) != 3) && f[0] == 3)
        power_control_clear_charger_test_burnin(pc);

    /* 3. switch_control: query "connected" (vtable+0x28). If 0 and changed,
     *    notify (vtable+0x10). */
    {
        char connected =
            ((char (*)(void *))(*sw_vt)[0x28 / sizeof(void *)])(swobj);
        if (connected == 0) {
            if (!(PC_have_status(pc) && f[0] == PC_last_state(pc)))
                ((void (*)(void *))(*sw_vt)[0x10 / sizeof(void *)])(swobj);
        }
    }

    /* 4. publish IsPrimaryInserted (image computes (f[2]&1)^1 via the override
     *    publisher) and warn when the primary battery is missing. */
    od_override_set_bool("IsPrimaryInserted", (uint8_t)((f[2] & 1) ^ 1));
    if ((f[2] & 1) == 0)
        common_logf(PC_SRC, 0x49, LOG_ERR, "Primary battery not detected");

    /* 5. cache state + insertion bits (atomic release stores in the image). */
    PC_last_state(pc)  = f[0];
    PC_have_status(pc) = 1;
    PC_insdet_fail(pc) = (uint8_t)((f[2] >> 2) & 1);
    PC_inserted(pc)    = (uint8_t)(f[2] & 1);

    /* 6. dispatch per-state hook (only once power-control is initialized). */
    if (!PC_initialized(pc)) {
        common_logf(PC_SRC, 0x50, LOG_INFO,
                    "power control not initialized yet, ignoring callbacks");
        return;
    }

    void (***vt)(void *) = (void (***)(void *))pc; /* pc's own vtable at *(pc) */
    switch (f[0]) {
    case 0: ((void (*)(void *))(*vt)[0x10 / sizeof(void *)])(pc); break; /* off */
    case 1: ((void (*)(void *))(*vt)[0x18 / sizeof(void *)])(pc); break; /* on  */
    case 3: ((void (*)(void *))(*vt)[0x20 / sizeof(void *)])(pc); break; /* standby */
    case 5: ((void (*)(void *))(*vt)[0x28 / sizeof(void *)])(pc); break; /* identify */
    default:
        common_logf(PC_SRC, 0x61, LOG_ERR, "Not supported yet");
        break;
    }
}

/* ------------------------------------------------------------------------ *
 * OD subscription binding (OEM 0x133c20, PowerControl ctor) — DOCS
 * ------------------------------------------------------------------------ *
 *
 * The PowerControl constructor (PowerService+0x50) binds these OD signal
 * callbacks. The body is STL std::vector<callback>/std::function growth and is
 * not reconstructed here; the bindings are:
 *
 *   "power_control_state"                   -> power_control_on_state (0x133820)
 *   "power_control_control"                 -> identity manager (0x125820)
 *   "power_control_measurements"            -> identity manager (0x135830)
 *   "power_pedal_power_switch_control_init" -> identity manager (0x125820)
 *   "battery_primary_battery_state_init"    -> identity manager (0x125820)
 *
 * It finishes by zeroing PC_have_status, clearing the charge accumulators
 * (PowerControl+0x4c/+0x54), setting +0x5c=1, and calling
 * power_control_clear_charger_test_burnin() once at startup.
 */
