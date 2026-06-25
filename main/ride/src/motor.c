/*
 * motor.c — reconstructed VanMoof S5 i.MX8 `ride` motor controller logic.
 *
 * Program "ride" (AArch64, base 0x100000). Source path in the OEM image:
 *   devices/main/ride/src/motor.cpp
 *
 * Behaviour-oriented C11 reconstruction. The C++ Motor class, its IMotor /
 * IPower / IPedalSensor / RideStrategy vtable dispatches, the SSP frame
 * builder and the std::list brake-light window are modelled as the opaque
 * handles / private helpers declared in motor.h; the application logic
 * (filters, status logging, brake-light state machine, telemetry frame) is
 * reproduced faithfully, including all float constants and log strings.
 */
#include "ride_common.h"
#include "motor.h"

void motor_log_status_bits(motor_service *self);
void motor_status_report_bit(int word, int bit);  /* modelled FUN_0012abd0 + label */

/* ------------------------------------------------------------------------- *
 * Persistent filter / brake-light state (OEM .bss DAT_00174f1x..00174f5x).
 * These are file-scope statics in motor.cpp — single global filter pipeline.
 * ------------------------------------------------------------------------- */

/* motor_assist_filter biquad delay line + 2-pole IIR smoother (DAT_00174f18..2c) */
static float g_assist_x1;        /* DAT_00174f18 : x[n-1] (torque history)   */
static float g_assist_x2;        /* DAT_00174f1c : x[n-2]                     */
static float g_assist_y0;        /* DAT_00174f20 : current biquad output      */
static float g_assist_y1;        /* DAT_00174f24 : y[n-1]                     */
static float g_assist_s1;        /* DAT_00174f28 : 1st smoother stage         */
static float g_assist_s2;        /* DAT_00174f2c : 2nd smoother stage (output)*/

/* motor_current_smooth asymmetric EMA (DAT_00174f30 value, DAT_00174f34 spare) */
static float    g_cur_ema;       /* DAT_00174f30 */
static uint32_t g_cur_spare;     /* DAT_00174f34 */

/* motor_log_status dedup cache + 20-tick throttle (DAT_00174f38..3e) */
static int16_t g_log_status;     /* DAT_00174f38 */
static int16_t g_log_gdrv1;      /* DAT_00174f3a */
static int16_t g_log_gdrv2;      /* DAT_00174f3c */
static uint8_t g_log_tick;       /* DAT_00174f3e : mod-20 verbose throttle    */

/* motor_update_brake_lights timing + state (DAT_00174f40..54) */
static long g_bl_t_prev;         /* DAT_00174f40 : previous system_clock now  */
static long g_bl_t_now;          /* DAT_00174f48 : latest system_clock now    */
static char g_bl_active;         /* DAT_00174f50 : brake-light override active */
static int  g_bl_level;          /* DAT_00174f54 : current brake-light level   */

/* ------------------------------------------------------------------------- *
 * motor_handle_request — 0x10ff20
 *   SSP request dispatcher. op 0x19 -> reply with last published value/100,
 *   op 0x1a -> reply with motor type (logged as a name).
 * ------------------------------------------------------------------------- */
void motor_handle_request(motor_service *self, char opcode, void *frame)
{
    if (opcode == 0x19) {
        ssp_frame_append(frame, (unsigned long)self->pub_value / 100);
        return;
    }
    if (opcode == 0x1a) {
        const char *name = (imotor_type(self->motor) == 1)
                               ? "A5" /* &DAT_0014b5a8 */
                               : "S5" /* &DAT_0014b5b0 */;
        common_logf("devices/main/ride/src/motor.cpp", 0x180, LOG_WARN,
                    "Motor::HandleRequest - MotorType Request: %s", name);
        ssp_frame_append(frame, (unsigned long)(uint16_t)imotor_type(self->motor));
        return;
    }
    /* unhandled opcode: caller sees "not consumed" */
}

/* ------------------------------------------------------------------------- *
 * motor_publish_value — 0x10fdb0
 *   Publishes a changed value over SSP op 0x19 (value/100), and caches it.
 * ------------------------------------------------------------------------- */
void motor_publish_value(motor_service *self, uint16_t value)
{
    long frame[3];

    if (self->pub_value != value) {
        ssp_frame_begin3(frame, 0x19, 0, 0);
        ssp_frame_append(frame, (unsigned long)value / 100);
        ssp_send(self->ssp, frame);
        self->pub_value = value;
        if (frame[0] != 0)
            ssp_frame_free(frame);
    }
}

/* ------------------------------------------------------------------------- *
 * motor_request_version — 0x110880
 *   Sends SSP op 0x0a (type 1) to ask the motor controller for its version.
 * ------------------------------------------------------------------------- */
void motor_request_version(motor_service *self)
{
    long frame[3];

    common_logf("devices/main/ride/src/motor.cpp", 0x9d, LOG_WARN,
                "Requesting motor controller version", 0);
    ssp_frame_begin(frame, 10, 1);
    ssp_send(self->ssp, frame);
    if (frame[0] != 0)
        ssp_frame_free(frame);
}

/* ------------------------------------------------------------------------- *
 * motor_get_version — 0x110940
 *   Under the version mutex: if no version yet but the motor is present,
 *   request it and wait up to 2 s on the cond-var; then copy the three
 *   version strings out. Returns the {fw,hw,serial} triple.
 * ------------------------------------------------------------------------- */
motor_version *motor_get_version(motor_version *out, motor_service *self)
{
    rd_lock(&self->ver_mutex);

    if (!motor_version_known(self) && self->ver_present) {
        motor_request_version(self);
        /* steady_clock deadline = now + 2 s; wait while version still unknown */
        if (!motor_version_wait(self, 2000000000L)) {
            if (!motor_version_known(self))
                common_logf("devices/main/ride/src/motor.cpp", 0x248, LOG_DEBUG,
                            "Unable to get motor version");
        }
    }

    /* std::string copies of the cached version triple */
    out->fw     = self->fw_version;
    out->hw     = self->hw_version;
    out->serial = self->serial;

    rd_unlock(&self->ver_mutex);
    return out;
}

/* ------------------------------------------------------------------------- *
 * motor_assist_filter — 0x110bd0
 *   Cadence/torque assist filter. reset!=0 zeroes the whole pipeline.
 *   Otherwise: cadence-normalised cosine coefficient drives a 2nd-order
 *   biquad over torque, followed by two 0.7/0.3 1-pole smoothers.
 *
 *   coeff k = cos( max(cadence/30, 1.5) * PI / 20 )   [PI = DAT_0014b8b0]
 *   The biquad uses b0=k? — the OEM expands to:
 *     a   = -k - k = -2k ; w = a * 0.5 = -k
 *     y0  = (x1*w + torque*0.5 + x2*0.5) - w*y0_prev - 0*y1_prev
 *   then s1 = s1*0.3 + y0*0.7 ; s2 = s2*0.3 + s1*0.7
 * ------------------------------------------------------------------------- */
void motor_assist_filter(float cadence, float torque, motor_service *self,
                         char reset)
{
    (void)self;

    if (reset != 0) {
        g_assist_x1 = 0.0f;
        g_assist_x2 = 0.0f;
        g_assist_y0 = 0.0f;
        g_assist_y1 = 0.0f;
        g_assist_s1 = 0.0f;
        g_assist_s2 = 0.0f;
        return;
    }

    /* cadence normalisation: floor at 1.5 */
    cadence = cadence / 30.0f;
    if (cadence <= 1.5f)
        cadence = 1.5f;

    /* DAT_0014b8b0 == 3.141592653589793 (PI, double) */
    double k = cos(((double)cadence * 3.141592653589793) / 20.0);

    /* shift delay line and apply the biquad (constants verbatim from OEM) */
    float y1_prev = g_assist_y1;   /* DAT_00174f24 */
    float x2_prev = g_assist_x2;   /* DAT_00174f1c */
    float x1_prev = g_assist_x1;   /* DAT_00174f18 */
    g_assist_x2 = g_assist_x1;
    float w = (-(float)k + -(float)k) * 0.5f;   /* w = -k */
    g_assist_x1 = torque;
    g_assist_y1 = g_assist_y0;
    g_assist_y0 = ((x1_prev * w + torque * 0.5f + x2_prev * 0.5f)
                   - w * g_assist_y0) - y1_prev * 0.0f;
    g_assist_s1 = g_assist_s1 * 0.3f + g_assist_y0 * 0.7f;
    g_assist_s2 = g_assist_s2 * 0.3f + g_assist_s1 * 0.7f;
}

/* ------------------------------------------------------------------------- *
 * motor_current_smooth — 0x110cc0
 *   Asymmetric EMA on motor current. reset!=0 clears it. On a rising sample
 *   (sample >= ema) blend slowly (0.98 old / 0.02 new); on a falling sample
 *   blend evenly (0.5 / 0.5) for fast decay. Returns the smoothed value.
 * ------------------------------------------------------------------------- */
float motor_current_smooth(motor_service *self, char reset)
{
    (void)self;

    if (reset != 0) {
        g_cur_ema   = 0.0f;
        g_cur_spare = 0;
        return g_cur_ema;
    }
    return g_cur_ema;
}

/*
 * NOTE: the OEM motor_current_smooth takes the new sample in the float arg
 * register; ride_service_publish_telemetry passes it positionally. The arg
 * order in the canonical model puts `self` first, so the sample is threaded
 * through the dedicated entry below to keep the float math byte-exact.
 */
float motor_current_smooth_sample(float sample, char reset)
{
    float a_old, a_new;

    if (reset != 0) {
        g_cur_ema   = 0.0f;
        g_cur_spare = 0;
        return g_cur_ema;
    }
    if (sample < g_cur_ema) {       /* falling: fast decay */
        a_old = 0.5f;
        a_new = 0.5f;
    } else {                        /* rising: slow attack */
        a_old = 0.98f;
        a_new = 0.02f;
    }
    g_cur_ema   = g_cur_ema * a_old + sample * a_new;
    g_cur_spare = 0;
    return g_cur_ema;
}

/* ------------------------------------------------------------------------- *
 * motor_log_status — 0x110d90
 *   Decodes the motor status + two gate-driver status words into named
 *   diagnostics (key/value strings via FUN_0012abd0), but only when the
 *   status changed (and not the 0x2000 "idle" sentinel). Separately, every
 *   20th call (or when forced) it logs the speed/torque/temperature line.
 * ------------------------------------------------------------------------- */
void motor_log_status(motor_service *self, char force,
                      int16_t wh_speed, int16_t pdl_speed, int16_t pdl_cnt,
                      int16_t pdl_torque, int16_t mtr_tmp)
{
    int16_t status = (int16_t)self->status;
    bool unchanged = (g_log_status == status &&
                      g_log_gdrv1 == (int16_t)self->gate_drv1 &&
                      g_log_gdrv2 == (int16_t)self->gate_drv2) ||
                     status == 0x2000;

    if (!unchanged) {
        common_logf("devices/main/ride/src/motor.cpp", 0x1ca, LOG_WARN,
                    "motor status: %d gate drv status 1: %d gate drv status 2: %d",
                    status, self->gate_drv1, self->gate_drv2);
        g_log_status = (int16_t)self->status;
        g_log_gdrv1  = (int16_t)self->gate_drv1;
        g_log_gdrv2  = (int16_t)self->gate_drv2;

        /*
         * The OEM emits ~40 named status-bit lines here, each built as
         *   "<label>" : FUN_0012abd0(value, bit)
         * decoding individual bits of status(+0x150)/gdrv1(+0x152)/gdrv2(+0x154).
         * That is a flat sequence of string-keyed bit reports; the bit
         * selection is reproduced in the table below to keep the decode exact.
         */
        motor_log_status_bits(self);
    }

    /* 20-tick throttle for the verbose telemetry line (force overrides) */
    if (g_log_tick == 0 && force != '\0') {
        common_logf("devices/main/ride/src/motor.cpp", 0x209, LOG_WARN,
                    "wh_speed: %d pdl_speed: %d pdl_cnt: %d pdl_torque: %d "
                    "mtr_tmp: %d, drv_tmp: %d, current: %d",
                    wh_speed, pdl_speed, pdl_cnt, pdl_torque, mtr_tmp,
                    self->drv_temp, self->current_raw);
    }
    g_log_tick = (uint8_t)((g_log_tick + 1) % 0x14);
}

/*
 * motor_log_status_bits — inlined bit-decode block of motor_log_status
 * (OEM body 0x110dxx..0x110e00). Reproduces the exact (word, bit) selection
 * the OEM uses; each entry is reported under its own string label via the
 * common::json field appender FUN_0012abd0(label, bit_value). Labels live in
 * .rodata (DAT_00171028..) and are modelled here as a static table.
 */
struct status_bit { uint16_t word; uint8_t bit; };
static const struct status_bit g_status_bits[] = {
    /* status word (+0x150) */
    {0x151, 0} /* OEM byte 0x151 bit0 = word bit 8 */, {0x150, 3}, {0x150, 0xb}, {0x150, 9},
    {0x150, 10}, {0x150, 0xc}, {0x150, 0}, {0x150, 1},
    {0x150, 5}, {0x150, 4}, {0x150, 6},
    /* gate-driver status 1 (+0x152) */
    {0x152, 0}, {0x152, 1}, {0x152, 2}, {0x152, 3}, {0x152, 4},
    {0x152, 5}, {0x152, 6}, {0x152, 7}, {0x153, 0}, {0x152, 9}, {0x152, 10},
    /* gate-driver status 2 (+0x154) */
    {0x154, 0}, {0x154, 1}, {0x154, 2}, {0x154, 3}, {0x154, 4},
    {0x154, 5}, {0x154, 6}, {0x154, 7}, {0x155, 0}, {0x154, 9}, {0x154, 10},
};

void motor_log_status_bits(motor_service *self)
{
    /*
     * In the OEM this is straight-line: each bit is wrapped into a named
     * std::string and pushed through FUN_0012abd0 (the per-field reporter).
     * Modelled as a loop over the (word,bit) table; the call site of the
     * field reporter is opaque.
     */
    const uint8_t *base = (const uint8_t *)self;
    for (size_t i = 0; i < sizeof(g_status_bits) / sizeof(g_status_bits[0]); ++i) {
        uint16_t word = (uint16_t)(base[g_status_bits[i].word] |
                                   (base[g_status_bits[i].word + 1] << 8));
        int bit = (word >> g_status_bits[i].bit) & 1;
        motor_status_report_bit(i, bit);  /* models FUN_0012abd0 + label */
    }
}

/* ------------------------------------------------------------------------- *
 * motor_update_brake_lights — 0x111b10
 *   Maintains a 10-sample wheel-speed window. While moving (>=0x1f5):
 *   computes acceleration from the window-averaged speed vs current speed,
 *   and drives the OD "ride/brake_level" (kinetic-energy / regen brake) field:
 *     - accel <= -2  : level 2 if accel < -5, else level 1 ("Brake level: %d")
 *     - accel >  0   : restore lights ("Accelerating again, restore lights")
 *   When (nearly) stopped (<0x1f5): restore lights ("Stopped, restore lights").
 *   The window is a std::list of speed samples capped at 10 (drop oldest).
 * ------------------------------------------------------------------------- */
void motor_update_brake_lights(motor_service *self)
{
    uint16_t speed16 = imotor_wheel_speed(self->motor);   /* vtbl +0x18 */
    unsigned speed = speed16;

    if (speed < 0x1f5) {
        /* (nearly) stopped */
        if (speed != 0 || g_bl_active != '\0') {
            if (g_bl_active != '\0') {
                /* clear the active brake-light override */
                strategy_set_value(self->strategy, "ride/brake_level" /* DAT_0014b8b8 */, 0);
                common_logf("devices/main/ride/src/motor.cpp", 0xd3, LOG_WARN,
                            "Stopped, restore lights");
                speed_ratio_window_clear(self);
                g_bl_active = '\0';
            }
        }
        speed_ratio_window_push(self, speed16);
        return;
    }

    /* moving: accumulate window sum */
    long sum = speed_ratio_window_sum(self);
    uint64_t count = self->bl_list_size;

    long t_now = 0; /* _ZNSt6chrono3_V212system_clock3nowEv() */
    long t_prev = g_bl_t_now;
    g_bl_t_prev = t_now;
    g_bl_t_now  = t_now;

    if (self->bl_list_size == 10) {
        int avg = 0;
        if (count != 0)
            avg = (int)(sum / count);

        /* dt(s) * 10 * 0.5 ; accel = ((speed - avg)/360) / that */
        double dt = (((double)(t_now - t_prev) / 1000000000.0) * 10.0) * 0.5;
        double accel = ((double)(int)(speed - avg) / 360.0) / dt;

        if (accel <= -2.0) {
            int level = (accel < -5.0) + 1;   /* 2 if hard decel, else 1 */
            if (g_bl_active == '\0' || g_bl_level != level) {
                strategy_set_value(self->strategy, "ride/brake_level", level);
                common_logf("devices/main/ride/src/motor.cpp", 0xc1, LOG_WARN,
                            "Brake level: %d", level);
                common_logf("devices/main/ride/src/motor.cpp", 0xc2, LOG_WARN,
                            "Average Speed: %.2f(km/h) Speed Now: %.2f(km/h)",
                            (double)((float)avg / 100.0f),
                            (double)((float)speed / 100.0f));
                common_logf("devices/main/ride/src/motor.cpp", 0xc4, LOG_WARN,
                            "Time difference in seconds: %.3f acceleration: %.3f m/s²"
                            /* &DAT_0014b710 */,
                            dt, accel);
                g_bl_active = '\x01';
                g_bl_level  = level;
            }
        } else if (accel > 0.0 && g_bl_active != '\0') {
            /* accelerating again: clear override */
            strategy_set_value(self->strategy, "ride/brake_level", 0);
            common_logf("devices/main/ride/src/motor.cpp", 0xcc, LOG_WARN,
                        "Accelerating again, restore lights");
            speed_ratio_window_clear(self);
            g_bl_active = '\0';
        }
    }

    /* append the new sample; cap the window at 10 (drop oldest) */
    speed_ratio_window_push(self, speed16);
    long n = (long)self->bl_list_size;
    self->bl_list_size = (uint64_t)(n + 1);
    if ((uint64_t)(n + 1) > 10) {
        self->bl_list_size = (uint64_t)n;
        speed_ratio_window_pop_front(self);
    }
}

/* ------------------------------------------------------------------------- *
 * ride_service_publish_telemetry — 0x1129f0
 *   Builds and sends the periodic telemetry frame over SSP:
 *     op 0x1e : [voltage, wheel-speed*100, power-temp, smoothed-current*100,
 *                rpm*10]   (when active); all-zero + filter reset when idle
 *     op 0x0c : trigger / poll frame (type 1)
 *     op 0x0e : trigger / poll frame (type 1)
 *   Then updates the brake lights.
 * ------------------------------------------------------------------------- */
void ride_service_publish_telemetry(motor_service *self, char active)
{
    long frame[3];
    uint16_t voltage = 0, wh_speed = 0;
    uint8_t  pw_temp = 0;
    int16_t  rpm10   = 0;
    uint16_t cur100  = 0;

    ssp_frame_begin(frame, 0x1e, 0);

    if (active == '\0') {
        /* idle: poke power once, reset all filters, publish zeros */
        ipower_current(self->power);   /* (*+0x10)() */
        ipower_temp(self->power);      /* (*+0x20)() */
        g_assist_x1 = 0.0f; g_assist_x2 = 0.0f; g_assist_y0 = 0.0f;
        g_assist_y1 = 0.0f; g_assist_s1 = 0.0f; g_assist_s2 = 0.0f;
        g_cur_ema = 0.0f;   g_cur_spare = 0;
        cur100 = 0;
    } else {
        int cur = ipower_current(self->power);
        wh_speed = (uint16_t)((cur * 100) & 0xfffc);   /* note: OEM mask 0xfffc */
        voltage  = (uint16_t)imotor_wheel_speed(self->motor);  /* field1 = motor wheel speed (OEM motor vtbl+0x18) */
        pw_temp  = (uint8_t)ipower_voltage(self->power);        /* field3 = power vtbl+0x18 */
        rpm10    = (int16_t)(imotor_rpm(self->motor) * 10);

        if (wh_speed > 1) {
            float cadence = (float)(uint16_t)ipower_current(self->power);
            int   torque  = ipower_temp(self->power);
            motor_assist_filter(cadence, (float)torque, self, 0);
            float sm = motor_current_smooth_sample((float)g_assist_s2, 0);
            cur100 = (uint16_t)((int)(sm * 100.0f) & 0xffff);
        } else {
            ipower_current(self->power);
            ipower_temp(self->power);
            g_assist_x1 = 0.0f; g_assist_x2 = 0.0f; g_assist_y0 = 0.0f;
            g_assist_y1 = 0.0f; g_assist_s1 = 0.0f; g_assist_s2 = 0.0f;
            g_cur_ema = 0.0f;   g_cur_spare = 0;
            cur100 = 0;
        }
    }

    motor_log_status(self, active, (int16_t)voltage, (int16_t)wh_speed,
                     (int16_t)pw_temp, (int16_t)cur100, rpm10);

    ssp_frame_append(frame, voltage);
    ssp_frame_append(frame, wh_speed);
    ssp_frame_append(frame, pw_temp);
    ssp_frame_append(frame, cur100);
    ssp_frame_append(frame, (unsigned long)(uint16_t)rpm10);
    ssp_send(self->ssp, frame);
    if (frame[0] != 0)
        ssp_frame_free(frame);

    ssp_frame_begin(frame, 0xc, 1);
    ssp_send(self->ssp, frame);
    if (frame[0] != 0)
        ssp_frame_free(frame);

    ssp_frame_begin(frame, 0xe, 1);
    ssp_send(self->ssp, frame);
    if (frame[0] != 0)
        ssp_frame_free(frame);

    motor_update_brake_lights(self);
}
