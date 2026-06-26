/*
 * ble.h — BLE component supervisor private declarations (class 3BLE:9Component).
 * Included after monitor_common.h.
 */
#ifndef MONITOR_BLE_H
#define MONITOR_BLE_H

#include "monitor_common.h"

/*
 * BLE component object (class 3BLE:9Component), OEM size 360 (0x168) bytes.
 * Layout mirrors the offsets observed in the OEM disassembly; std::string
 * members are modelled as opaque byte handles here.
 */
struct ble_component {
    struct icomponent base;   /* +0x00 vtable, +0x08 mqtt (IComponent base) */
    uint8_t  m_alive;         /* +0x60 is_alive cached flag                 */
    int64_t  m_start_tick;    /* +0x70 monotonic ns start tick (get_value)  */
    uint8_t  m_name_state;    /* +0x78                                      */
    void    *m_reset_reason;  /* +0x80 std::string ble/system/reset_reason  */
    void    *m_status_field2; /* +0xa0 std::string status field 2           */
    void    *m_status_field3; /* +0xc0 std::string status field 3           */
    void    *m_version;       /* +0xe0 std::string ble/system/version_info  */
    void    *m_name;          /* component name std::string ("ble")         */
    void    *m_vars_json;     /* +0x128 region: serialised vars for ble/vars */
};

/* 0x60-byte status object returned by get_status: three std::strings. */
struct ble_status {
    void *reset_reason;
    void *field2;
    void *field3;
};

/* OEM vtable @ 0x171070 (restored by the destructor). */
extern const struct icomponent_ops ble_component_vtable;

/* IComponent base default aliveness (slot @ 0x12a990). */
bool icomponent_base_is_alive(struct icomponent *self);

/* STL string helpers (modelled glue; no instruction-level transcription). */
void op_string_copy(char *out, void *src);
void op_string_set(char *out, const char *literal);
void op_string_dtor(void *str);

/* Component method prototypes (vtable slots). */
void  ble_get_name(struct ble_component *self, char *out_name);
void  ble_get_version(struct ble_component *self, char *out_version);
void  ble_get_status(struct ble_component *self, struct ble_status *out_status);
bool  ble_is_alive(struct ble_component *self);
float ble_get_value(struct ble_component *self, const int64_t *clock);
void  ble_get_type(struct ble_component *self, char *out_type);
void  ble_publish(struct ble_component *self);
void  ble_dtor(struct ble_component *self);

#endif /* MONITOR_BLE_H */
