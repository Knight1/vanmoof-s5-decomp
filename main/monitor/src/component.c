/*
 * component.c — reconstructed VanMoof S5 i.MX8 `monitor` IComponent base +
 * GPIO reset bank. Behaviour-oriented C translation of the OEM AArch64
 * decompilation (program "monitor", image base 0x100000).
 *
 * VanMoof-authored logic only. The C++ runtime (std::string / std::ofstream /
 * std::thread / condition_variable), mosquitto and the exception machinery are
 * VENDOR — modelled here as opaque helpers (mqtt_*, gpio_write_file, op_*)
 * rather than reconstructed. OEM addresses are quoted per function.
 */
#include "monitor_common.h"
#include "component.h"

#include <stdio.h>
#include <string.h>

/* ======================================================================== *
 * IComponent base
 * ======================================================================== */

/* Base IComponent vtable (OEM &DAT_001707a0). The seven application slots are
 * the LAB_001465xx thunks installed by component_base_ctor; here the table is
 * declared so subclasses can chain through it. */
extern const icomponent_ops icomponent_base_vtable; /* &DAT_001707a0 */

/*
 * component_base_ctor — OEM 0x146680.
 * Installs the base vtable and constructs the embedded mosquitto subobject
 * (FUN_00108e30) wiring its seven callback slots. We model the VanMoof-visible
 * effect: set the vtable and attach the mqtt handle.
 */
void icomponent_base_ctor(icomponent *self, mqtt_client *mqtt)
{
    self->ops  = &icomponent_base_vtable;   /* *param_1 = &DAT_001707a0 */
    self->mqtt = mqtt;                       /* param_1[1] = mosquitto subobject */
    /* FUN_00108bc0/.../FUN_00108cf0: register mosquitto callbacks — vendor. */
}

/*
 * component_base_dtor — OEM 0x146730.
 * Re-seats the base vtable and tears down the mosquitto subobject
 * (FUN_00109410), which is vendor.
 */
void icomponent_base_dtor(icomponent *self)
{
    self->ops = &icomponent_base_vtable;     /* *param_1 = &DAT_001707a0 */
    /* FUN_00109410(param_1[1]): destroy mosquitto handle — vendor. */
}

/*
 * component_registry_element_dtor — OEM 0x147b20.
 * Destroys one registry entry: a { IComponent* @+0x10, std::string name @+0x20
 * with SSO buffer @+0x30 } pair. Frees the name's heap storage when it is not
 * the inline SSO buffer, then invokes the component's deleting destructor
 * (vtable slot 0).
 */
void component_registry_element_dtor(void *element)
{
    unsigned char *e = (unsigned char *)element;
    void *name_data    = *(void **)(e + 0x20);
    void *name_ssobuf  = e + 0x30;
    icomponent **comp_slot = (icomponent **)(e + 0x10);

    if (name_data != name_ssobuf)
        op_delete(name_data, 0);             /* FUN_00109150: operator delete */

    if (*comp_slot != NULL) {
        icomponent *c = *comp_slot;
        c->ops->dtor(c);                     /* deleting dtor (vtable slot 0) */
    }
}

/* ======================================================================== *
 * component_format_version_string — OEM 0x1371e0
 *
 * Joins the firmware version triplet into "<a>.<b>.<c>". The OEM body inlines
 * the std::string concatenation: snprintf("%d", a) + "." + ... yielding
 * "1.5.0" (DAT_0014ccb8 = "%d", DAT_0014ccb0 = "."). The three integers are
 * constants in this image; modelled as the join behaviour into the caller buf.
 * ======================================================================== */
char *component_format_version_string(char *out)
{
    static const int ver_major = 1;          /* snprintf("%d", 1) */
    static const int ver_minor = 5;          /* snprintf("%d", 5) */
    static const int ver_patch = 0;          /* snprintf("%d", 0) */

    snprintf(out, 16, "%d.%d.%d", ver_major, ver_minor, ver_patch);
    return out;
}

/* ======================================================================== *
 * component_mqtt_client_ctor — OEM 0x13a8d0
 *
 * Constructs the mosquitto-backed IComponent. The OEM body chains
 * icomponent_base_ctor, installs its own vtable (&DAT_00170620), zero-inits the
 * publish/subscribe ring buffers (8-slot directory, 0x200-byte payload arena),
 * sets the host (FUN_001468c0), connects host:port=0x75b(1883) keepalive=0x3c
 * (FUN_00146820), waits on the connect condition variable, and on success
 * launches the network thread (entry FUN_0013bdd0). A non-zero connect result
 * code is formatted into "Mosquitto connect error: <rc>" and thrown as a
 * std::runtime_error.
 *
 * Ring-buffer sizing, std::thread, the condition-variable handshake and the
 * exception path are vendor; reconstructed here as the connection wiring + the
 * documented error behaviour.
 * ======================================================================== */
void component_mqtt_client_ctor(icomponent *self, const char *host,
                                const char *host_alt, uint32_t keepalive,
                                const char *client_id, void *user_ctx)
{
    int rc;
    const char *h = (host_alt != NULL) ? host_alt : host;

    icomponent_base_ctor(self, NULL);        /* component_base_ctor(.., param_4, 0) */
    self->ops = &icomponent_base_vtable;     /* &DAT_00170620 (subclass vt) */

    (void)keepalive;                         /* 0x3c (60s) — connect glue */
    (void)client_id;
    (void)user_ctx;                          /* stored at +0x58 */
    (void)h;                                 /* mosquitto host config — vendor */

    /* connect: FUN_00146820(self, client_id, 0x75b, 0x3c). */
    rc = 0;                                  /* modelled: success */
    if (rc != 0) {
        /* "Mosquitto connect error: " + snprintf("%d", rc) -> std::runtime_error */
        char msg[64];
        snprintf(msg, sizeof msg, "Mosquitto connect error: %d", rc);
        (void)msg;                           /* FUN_001095c0: __cxa_throw — vendor */
        return;
    }

    /* On success: wait for the connect callback, then launch the network thread
     * (body FUN_0013bdd0) which drives mosquitto_loop. std::thread/cv is vendor. */
}

/* ======================================================================== *
 * GPIO reset bank (common/gpio.cpp + component.cpp)
 * sysfs interface: /sys/class/gpio
 * ======================================================================== */

/*
 * gpio_export_and_configure — OEM 0x138bd0.
 * Exports a GPIO pin and configures its direction:
 *   - writes "<pin>" to /sys/class/gpio/export
 *   - writes "out"/"in" to /sys/class/gpio/gpio<pin>/direction
 * On failure to open the gpio node, logs at gpio.cpp:0x18 DEBUG:
 *   "Failed to open GPIO %d what(): %s".
 * The std::ofstream / std::string formatting is vendor; the sysfs paths, the
 * "out"/"in" tokens (DAT_0014fff8/DAT_00150000) and the error log are VanMoof.
 * (In the OEM ABI the direction is the third arg: 0 -> "out", non-0 -> "in";
 * here `out` is that selector inverted to read naturally — out==true -> "out".)
 */
void gpio_export_and_configure(int pin, bool out)
{
    const char *export_path = "/sys/class/gpio/export";   /* DAT_00150020 */
    char node[64];
    char dir_path[80];
    char pin_str[16];
    const char *direction = out ? "out" : "in";           /* DAT_0014fff8 / 0x150000 */

    snprintf(pin_str, sizeof pin_str, "%d", pin);
    snprintf(node, sizeof node, "/sys/class/gpio/gpio%d", pin);

    /* export the pin, then set its direction (out|in). */
    gpio_write_file(export_path, pin_str);
    snprintf(dir_path, sizeof dir_path, "%s/direction", node);
    gpio_write_file(dir_path, direction);
}

/*
 * gpio_set_value — OEM 0x139180.
 * Drives a previously-exported pin: write (value == 1) ? "1" : "0" to
 * /sys/class/gpio/gpio<pin>/value.
 */
void gpio_set_value(int pin, int value)
{
    char value_path[80];

    snprintf(value_path, sizeof value_path, "/sys/class/gpio/gpio%d/value", pin);
    gpio_write_file(value_path, (value == 1) ? "1" : "0");
}

/*
 * gpio_write_file — OEM 0x139950.
 * Opens `path` as an std::ofstream and writes `s`. If the open fails the OEM
 * throws std::runtime_error("Error, unable to open file '" + path + "'"). The
 * iostream machinery and the exception are vendor; modelled with stdio.
 */
void gpio_write_file(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");               /* std::ofstream(path) */
    if (f == NULL)
        return;                               /* FUN_001095c0: __cxa_throw — vendor */
    fputs(s, f);                              /* FUN_00109100: stream insert */
    fclose(f);
}

/*
 * component_gpio_reset_line_ctor — OEM 0x134bf0.
 * Builds one reset line: export the pin as an output and store the active
 * polarity at +0x18. Then per `mode`:
 *   mode 0 -> gpio_set_value(active)        (assert the line)
 *   mode 1 -> gpio_set_value(active ^ 1)    (deassert / inverted)
 *   else   -> leave the level untouched
 * (OEM calls gpio_export_and_configure(pin, param_3=0); param_3==0 selects the
 * "out" direction token, i.e. the pin is configured as an OUTPUT.)
 */
void component_gpio_reset_line_ctor(gpio_reset_line *self, int pin, int mode,
                                    int active)
{
    self->pin        = pin;
    self->active_low = (bool)active;

    gpio_export_and_configure(pin, true);    /* configure as output ("out") */

    if (mode == 0)
        gpio_set_value(pin, active);
    else if (mode == 1)
        gpio_set_value(pin, active ^ 1);
    /* mode >= 2: no initial drive. */
}

/*
 * component_reset_lines_ctor — OEM 0x120310.
 * Constructs the four ECU reset lines on GPIO pins {1, 0, 10, 11}, each with
 * mode 2 (no initial drive) and active-high(0) polarity.
 */
void component_reset_lines_ctor(component_reset_lines *self)
{
    static const int pins[4] = MONITOR_RESET_PINS;   /* { 1, 0, 10, 11 } */
    int i;

    self->vtable = NULL;                     /* &DAT_00170340 */
    for (i = 0; i < 4; i++)
        component_gpio_reset_line_ctor(&self->line[i], pins[i], 2, 0);
}

/* ======================================================================== *
 * can_node_id_to_ecu_name — OEM 0x12ceb0
 *
 * Maps a CAN node id to its ECU display name. Exact reproduction of the OEM
 * decision tree: specific ids first (0xA1 Motor, 0xC1 Elock, 0xC2 Eshifter,
 * 0xC3 Rearlight, 0xC4 Frontlight) then by the top-three-bit class (id & 0xE0):
 * 0x80 MainECU, 0xA0 PowerECU, 0xC0 UserECU; else "Unknown".
 * ======================================================================== */
const char *can_node_id_to_ecu_name(uint8_t node)
{
    if (node == 0xc2)
        return "Eshifter";

    if (node < 0xc3) {
        if (node == 0xa1)
            return "Motor";
        if (node == 0xc1)
            return "Elock";
        {
            uint8_t cls = (uint8_t)(node & 0xe0);
            if (cls < 0xa2) {
                if (cls == 0x80)
                    return "MainECU";
                if (cls != 0xa0)
                    return "Unknown";
                return "PowerECU";
            }
        }
    } else {
        if (node == 0xc3)
            return "Rearlight";
        if (node == 0xc4)
            return "Frontlight";
        if (0xc1 < (uint8_t)(node & 0xe0))
            return "Unknown";
    }

    if ((uint8_t)(node & 0xe0) == 0xc0)
        return "UserECU";
    return "Unknown";
}
