/*
 * monitor / ble.c  -  BLE component supervisor (class 3BLE:9Component)
 *
 * AArch64 monitor service, image base 0x100000.
 * vtable @ 0x171070, object size 360 (0x168) bytes.
 *
 * Reconstructed behaviour-faithfully from the OEM disassembly of the
 * component methods (created in Ghidra: get_name 0x12b2a0, get_version
 * 0x12ba50, get_status 0x12b980, is_alive 0x12aa40, get_value 0x12a9d0,
 * get_type 0x1268f0, publish 0x1271a0, dtor 0x126400). Pure STL / mosquitto /
 * nlohmann-json glue is modelled at the call site, not transcribed.
 *
 * Observed object layout (offsets from `this`):
 *   +0x60        uint8  m_alive          (is_alive cached flag)
 *   +0x70        int64  m_start_tick     (monotonic ns at start; get_value)
 *   +0x80/+0x88  std::string m_reset_reason   (ble/system/reset_reason)
 *   +0xe0/+0xe8  std::string m_version        (ble/system/version_info)
 *   +0x128       std::map    m_published      (heartbeat de-dup tracking)
 *
 * MQTT topics owned by this component:
 *   ble/system/version_info, ble/heartbeat, ble/system/reset_reason, ble/vars
 */

#include "monitor_common.h"
#include "ble.h"

/* ------------------------------------------------------------------ */
/* OEM 0x12b2a0 : BleComponent::get_name()                            */
/*   Returns the cached component name std::string ("ble").           */
/* ------------------------------------------------------------------ */
void ble_get_name(struct ble_component *self, char *out_name)
{
    op_string_copy(out_name, self->m_name);
}

/* ------------------------------------------------------------------ */
/* OEM 0x12ba50 : BleComponent::get_version()                         */
/*   Returns a copy of the cached version string @this+0xe0/+0xe8.     */
/*   The caller publishes the result to ble/system/version_info.      */
/* ------------------------------------------------------------------ */
void ble_get_version(struct ble_component *self, char *out_version)
{
    /* _M_construct(out, m_version.data, m_version.data + m_version.len) */
    op_string_copy(out_version, self->m_version);
}

/* ------------------------------------------------------------------ */
/* OEM 0x12b980 : BleComponent::get_status()                          */
/*   Builds a 0x60-byte status object out of three std::strings copied */
/*   from the object: m_reset_reason (+0x80), field2 (+0xa0),          */
/*   field3 (+0xc0).  Reset reason is published to                    */
/*   ble/system/reset_reason.                                         */
/* ------------------------------------------------------------------ */
void ble_get_status(struct ble_component *self, struct ble_status *out_status)
{
    op_string_copy(out_status->reset_reason, self->m_reset_reason);
    op_string_copy(out_status->field2, self->m_status_field2);
    op_string_copy(out_status->field3, self->m_status_field3);
}

/* ------------------------------------------------------------------ */
/* OEM 0x12aa40 : BleComponent::is_alive()                            */
/*   if (m_alive byte @+0x60 != 0) return false;                      */
/*   otherwise the slot tail-calls the shared base implementation      */
/*   (default IComponent::is_alive at 0x12a990).                       */
/* ------------------------------------------------------------------ */
bool ble_is_alive(struct ble_component *self)
{
    if (self->m_alive != 0)
        return false;
    return icomponent_base_is_alive((struct icomponent *)self);
}

/* ------------------------------------------------------------------ */
/* OEM 0x12a9d0 : BleComponent::get_value(seconds)                    */
/*   x3 = this->m_start_tick (+0x70); if 0 it is taken from arg.       */
/*   now = *clock; returns (float)(now - start) / 1e9f                 */
/*   i.e. elapsed seconds since start as a float (1e9f == 0x4e6e6b28). */
/* ------------------------------------------------------------------ */
float ble_get_value(struct ble_component *self, const int64_t *clock)
{
    int64_t start = self->m_start_tick;
    int64_t now;

    if (start == 0)
        start = clock[0];
    now = clock[0];
    return (float)(now - start) / 1.0e9f;    /* nanoseconds -> seconds */
}

/* ------------------------------------------------------------------ */
/* OEM 0x1268f0 : BleComponent::get_type()                            */
/*   Derives the component type name via an ostringstream pipeline     */
/*   (heavy STL glue). Modelled: returns the fixed type identifier.    */
/* ------------------------------------------------------------------ */
void ble_get_type(struct ble_component *self, char *out_type)
{
    (void)self;
    op_string_set(out_type, "ble");
}

/* ------------------------------------------------------------------ */
/* OEM 0x1271a0 : BleComponent::publish()                             */
/*   Periodic poll/publish entry. Walks the internal map (@+0x128) of  */
/*   seen heartbeats and publishes the component's MQTT topics.        */
/* ------------------------------------------------------------------ */
void ble_publish(struct ble_component *self)
{
    struct icomponent *base = (struct icomponent *)self;
    char buf[64];

    /* heartbeat: liveness beacon on every poll */
    mqtt_publish_str(base->mqtt, "ble/heartbeat", "1", 1, 0);

    /* version_info: cached firmware version string (this+0xe0) */
    ble_get_version(self, buf);
    mqtt_publish_str(base->mqtt, "ble/system/version_info", buf, 1, 0);

    /* reset_reason: first status field (this+0x80) */
    op_string_copy(buf, self->m_reset_reason);
    mqtt_publish_str(base->mqtt, "ble/system/reset_reason", buf, 1, 0);

    /*
     * ble/vars : the OEM body iterates the std::map at this+0x128 of reported
     * variables and serialises them as JSON. The map walk and nlohmann-json
     * serialisation are STL glue; modelled as the topic publish.
     */
    op_string_copy(buf, self->m_vars_json);
    mqtt_publish_str(base->mqtt, "ble/vars", buf, 1, 0);
}

/* ------------------------------------------------------------------ */
/* OEM 0x126400 : BleComponent::~BleComponent()                       */
/*   Re-seats the vtable to &ble_component_vtable (0x171070), then      */
/*   FLUSHES the component's three retained topics through the mqtt     */
/*   client (publish == mqtt vtable+0x18): ble/system/version_info      */
/*   (len 0x17), ble/heartbeat (len 0xd), ble/system/reset_reason       */
/*   (len 0x17). Finally it tail-calls the shared announce/teardown      */
/*   helper FUN_0012c5c0. (No teardown log / no member-string dtor in    */
/*   the OEM body — this publishes the final state on destruction.)      */
/* ------------------------------------------------------------------ */
void ble_dtor(struct ble_component *self)
{
    struct icomponent *base = (struct icomponent *)self;
    char buf[64];

    base->ops = &ble_component_vtable;       /* this->vtable = &BleComponent_vtable */

    /* flush the three retained topics (payloads = cached component state) */
    ble_get_version(self, buf);
    mqtt_publish_str(base->mqtt, "ble/system/version_info", buf, 1, 0);
    mqtt_publish_str(base->mqtt, "ble/heartbeat", "1", 1, 0);
    op_string_copy(buf, self->m_reset_reason);
    mqtt_publish_str(base->mqtt, "ble/system/reset_reason", buf, 1, 0);

    icomponent_base_dtor(base);              /* shared helper FUN_0012c5c0 */
}
