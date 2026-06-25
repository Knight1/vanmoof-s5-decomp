/*
 * speed_ratio_ride_strategy.c - VanMoof S5 i.MX8 `ride` service.
 *
 * SpeedRatioRideStrategy: the RideStrategy implementation that maps the
 * (region, assist_level, boost) triple to a motor command and serialises it
 * into SSP frames. Reconstructed from program "ride" (AArch64, base 0x100000).
 *
 * NOTE on the constructor address: the task brief cited 0x116450 as the
 * strategy ctor, but 0x116450 is a different object's constructor (it installs
 * vtable DAT_0016fc70/DAT_0016f178 and registers a "power_pedal_pedal" OD
 * handler -- that is RideService-side wiring, not RideStrategy). The genuine
 * SpeedRatioRideStrategy constructor is at 0x121ca0 (calls the RideStrategy
 * base ctor at 0x129780, then installs the derived vtable DAT_0016ff80); it is
 * invoked from main@0x10a7e0. This module reconstructs the real strategy.
 *
 * The four assist tables (default/region1/region2/region3) are .bss runtime
 * pointers (DAT_00175510/28/40/58); the numeric table contents are populated
 * at runtime and are NOT present in the image -- see the module header where
 * they are declared extern/off-image.
 */
#include "ride_common.h"
#include "speed_ratio_ride_strategy.h"

/* Region -> assist-table pointer dispatch (DAT_00175510/28/40/58, .bss). */
extern const uint8_t *speed_ratio_table_default; /* DAT_00175510 */
extern const uint8_t *speed_ratio_table_region1; /* DAT_00175528 */
extern const uint8_t *speed_ratio_table_region2; /* DAT_00175540 */
extern const uint8_t *speed_ratio_table_region3; /* DAT_00175558 */

/* derived/base vtable blobs, modelled as opaque externs. */
extern const void speed_ratio_vtable;      /* DAT_0016ff80 */
extern const void ride_strategy_vtable;    /* DAT_00170160 */

/* ---- helper: little-endian u16 read from a 14-byte assist-table entry ---- */
static uint16_t speed_ratio_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* =========================================================================
 * RideStrategy::RideStrategy (base ctor) -- OEM 0x129780
 *   Installs base vtable DAT_00170160, stores assist level at this+8, and
 *   zeroes the region (this+9) and boost (this+0xa) bytes (a single 16-bit
 *   store of 0 covers both).
 * ========================================================================= */
void ride_strategy_base_ctor(speed_ratio_strategy *self, uint8_t assist_level) {
    self->vptr = &ride_strategy_vtable;   /* DAT_00170160 */
    self->assist_level = assist_level;    /* this+8 */
    self->region = 0;                     /* this+9 */
    self->boost  = 0;                     /* this+0xa (16-bit zero store) */
}

/* =========================================================================
 * SpeedRatioRideStrategy::SpeedRatioRideStrategy (ctor) -- OEM 0x121ca0
 *   Chains the base ctor then installs the derived vtable DAT_0016ff80.
 *   Invoked from main@0x10a7e0.
 * ========================================================================= */
void speed_ratio_ride_strategy_ctor(speed_ratio_strategy *self, uint8_t assist_level) {
    ride_strategy_base_ctor(self, assist_level);
    self->vptr = &speed_ratio_vtable;     /* DAT_0016ff80 */
}

/* =========================================================================
 * SpeedRatioRideStrategy::~SpeedRatioRideStrategy (vtable +0x00) -- OEM 0x121b70
 *   Trivial / no-op base destructor.
 * ========================================================================= */
void speed_ratio_dtor(speed_ratio_strategy *self) {
    (void)self;
}

/* =========================================================================
 * SpeedRatioRideStrategy deleting destructor (vtable +0x08) -- OEM 0x121b80
 *   operator delete of the 0x10-byte object.
 * ========================================================================= */
void speed_ratio_deleting_dtor(speed_ratio_strategy *self) {
    op_delete(self, 0x10);   /* FUN_001097f0(self, 0x10) */
}

/* ---- trivial accessors (vtable slots) ---------------------------------- */

/* SetAssistLevel (vtable +0x10) -- OEM 0x1296f0 : write byte this+8 (0..4). */
void speed_ratio_set_assist_level(speed_ratio_strategy *self, uint8_t level) {
    self->assist_level = level;
}

/* GetAssistLevel (vtable +0x18) -- OEM 0x129700 : read byte this+8. */
uint8_t speed_ratio_get_assist_level(speed_ratio_strategy *self) {
    return self->assist_level;
}

/* SetRegion (vtable +0x20) -- OEM 0x129710 : write byte this+9. */
void speed_ratio_set_region(speed_ratio_strategy *self, uint8_t region) {
    self->region = region;
}

/* GetRegion (vtable +0x28) -- OEM 0x129720 : read byte this+9. */
uint8_t speed_ratio_get_region(speed_ratio_strategy *self) {
    return self->region;
}

/* SetBoost (vtable +0x38) -- OEM 0x129770 : write byte this+0xa. */
void speed_ratio_set_boost(speed_ratio_strategy *self, uint8_t boost) {
    self->boost = boost;
}

/* =========================================================================
 * SpeedRatioRideStrategy::ComputeCommand (vtable +0x48) -- OEM 0x121b90
 *
 * Core assist algorithm. Selects one of four per-region assist tables by the
 * region byte (read via GetRegion), then indexes it:
 *   - boost set        -> entry 5 at byte offset 0x46
 *   - otherwise        -> assist_level * 0xe (entry 0..4)
 * and returns the 14-byte motor command copied verbatim from that entry.
 *
 * Region dispatch (matching the OEM branch tree exactly):
 *   region == 1 -> table_region1 (DAT_00175528)
 *   region == 2 -> table_region2 (DAT_00175540)
 *   region == 3 -> table_region3 (DAT_00175558)
 *   region == 4 -> WARN "Unsupported region setting: %d" then fall through
 *   default/0   -> table_default  (DAT_00175510)
 *
 * Args (OEM ABI): param_2 = boost flag, param_3 = assist level. Note the
 * caller (GetFrames/GetCommandValue) passes the object's own boost byte
 * (this+0xa) and assist level (this+8); ComputeCommand re-reads the region
 * from the object via GetRegion rather than from an argument.
 * ========================================================================= */
motor_cmd speed_ratio_compute_command(speed_ratio_strategy *self,
                                      char boost, unsigned int assist_level) {
    const uint8_t *table;
    uint8_t reg;
    long index;
    motor_cmd cmd;
    const uint8_t *entry;

    reg = speed_ratio_get_region(self);

    /* OEM branch tree: reg==3 first, then reg<4 (1/2), then reg==4, else default. */
    if (reg == 3) {
        table = speed_ratio_table_region3;   /* DAT_00175558 */
    } else if (reg < 4) {
        if (reg == 1) {
            table = speed_ratio_table_region1;  /* DAT_00175528 */
        } else if (reg == 2) {
            table = speed_ratio_table_region2;  /* DAT_00175540 */
        } else {
            table = speed_ratio_table_default;  /* DAT_00175510 (reg 0) */
        }
    } else {
        if (reg == 4) {
            common_logf("devices/main/ride/src/speed_ratio_ride_strategy.cpp", 0x57, LOG_INFO,
                        "Unsupported region setting: %d", speed_ratio_get_region(self));
        }
        table = speed_ratio_table_default;      /* DAT_00175510 */
    }

    /* Index: boost -> entry 5 (0x46); else assist_level * 14. */
    index = (long)(assist_level & 0xff) * SPEED_RATIO_ENTRY_SIZE;
    if (boost != '\0') {
        index = SPEED_RATIO_BOOST_OFFSET;
    }

    /* Copy the 14-byte entry verbatim into the command struct (high 2 bytes 0). */
    entry = table + index;
    cmd.type = entry[0];
    cmd._b1  = entry[1];
    cmd.w0   = speed_ratio_rd16(entry + 2);
    cmd.w1   = speed_ratio_rd16(entry + 4);
    cmd.w2   = speed_ratio_rd16(entry + 6);
    cmd.w3   = speed_ratio_rd16(entry + 8);
    cmd.w4   = speed_ratio_rd16(entry + 10);
    cmd.w5   = speed_ratio_rd16(entry + 12);
    return cmd;
}

/* =========================================================================
 * SpeedRatioRideStrategy::GetCommandValue (vtable +0x30) -- OEM 0x129730
 *
 * Calls ComputeCommand (vtable +0x48) with the object's own boost (this+0xa)
 * and assist level (this+8), then returns a single u16 for telemetry:
 *   - cmd.type == 0 -> word at byte offset 2 (cmd.w0)
 *   - else          -> word at byte offset 8 (cmd.w3)
 * ========================================================================= */
unsigned int speed_ratio_get_command_value(speed_ratio_strategy *self) {
    motor_cmd cmd = speed_ratio_compute_command(self, (char)self->boost, self->assist_level);
    if (cmd.type == 0) {
        return cmd.w0;   /* auVar2._0_4_ >> 0x10 == word at offset 2 */
    }
    return cmd.w3;       /* auVar2._8_4_ & 0xffff == word at offset 8 */
}

/* =========================================================================
 * SpeedRatioRideStrategy::GetFrames (vtable +0x40) -- OEM 0x1297a0
 *
 * Calls ComputeCommand, then serialises one SSP frame into the returned
 * std::vector<ssp_buf> based on the command type byte:
 *
 *   type 1 : opcode 0x1c, payload = { w3, w4, w5 }      (3 x u16)
 *   type 2 : opcode 0x1b, payload = { w0, w1, w2, w4, w5, w3 } (6 x u16)
 *   else   : opcode 0x17, payload = { w0, w1, w2, w4, w5, w3 } (6 x u16)
 *
 * (The OEM passes auVar16 fields: type1 uses _8/_10/_12; type2/else use
 *  _2/_4/_6/_10/_12/_8 -- i.e. w0,w1,w2,w4,w5,w3.) The frame is appended to
 * the result vector via ssp_frame_begin / ssp_frame_append; the std::vector
 * grow/move glue is modelled by ssp_frame_vec_push.
 * ========================================================================= */
ssp_frame_vec *speed_ratio_get_frames(ssp_frame_vec *out, speed_ratio_strategy *self) {
    motor_cmd cmd;
    ssp_buf frame;

    out->begin = 0;
    out->end   = 0;
    out->cap   = 0;

    /* Indirect call through vtable +0x48, args = boost (this+0xa), level (this+8). */
    cmd = speed_ratio_compute_command(self, (char)self->boost, self->assist_level);

    if (cmd.type == 1) {
        ssp_frame_begin(&frame, 0x1c, 0);
        ssp_frame_append(&frame, cmd.w3);
        ssp_frame_append(&frame, cmd.w4);
        ssp_frame_append(&frame, cmd.w5);
    } else if (cmd.type == 2) {
        ssp_frame_begin(&frame, 0x1b, 0);
        ssp_frame_append(&frame, cmd.w0);
        ssp_frame_append(&frame, cmd.w1);
        ssp_frame_append(&frame, cmd.w2);
        ssp_frame_append(&frame, cmd.w4);
        ssp_frame_append(&frame, cmd.w5);
        ssp_frame_append(&frame, cmd.w3);
    } else {
        ssp_frame_begin(&frame, 0x17, 0);
        ssp_frame_append(&frame, cmd.w0);
        ssp_frame_append(&frame, cmd.w1);
        ssp_frame_append(&frame, cmd.w2);
        ssp_frame_append(&frame, cmd.w4);
        ssp_frame_append(&frame, cmd.w5);
        ssp_frame_append(&frame, cmd.w3);
    }

    /* push_back(std::move(frame)) into the result vector (grow/move glue). */
    ssp_frame_vec_push(out, &frame);

    if (frame.data != 0) {
        ssp_buf_free(&frame);   /* FUN_001097a0(local_20) */
    }
    return out;
}
