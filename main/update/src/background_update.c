#include "update_common.h"

/* ===== module-local framework model (externs + structs) ===== */
/* Concrete std::string model (libstdc++ std::__cxx11::string is 0x20 bytes:
   data ptr / size / SSO-or-capacity union). Embedded BY VALUE in bg_device_entry
   and bg_map_node, so it must be a complete type. */
typedef struct str { char data[128]; } str_t;

/* OD value read back from the publisher on remove (a CAN/OD payload union the
   OEM destructs by tag at 0x112e80..). Opaque blob; only allocated+freed here. */
typedef struct bg_od_value { unsigned char tag; unsigned char _pad[7]; void *body; } bg_od_value;

/*
 * BackgroundUpdate::DeviceEntry - one row of the retry table (operator_new 0x38).
 * Built by FUN_00111f30; offsets are the OEM byte offsets.
 */
typedef struct bg_device_entry {
    void   *publisher;        /* 0x00  OD/MQTT publisher handle (== bg->publisher) */
    str_t   topic;            /* 0x08  "update/background_update/progress_info/<name>" */
                              /*       (data 0x08, size 0x10, SSO 0x18) */
    int     remaining_tries;  /* 0x28  decremented on each FailRetry            */
    unsigned result_state;    /* 0x2c  0 Idle 1 FailRetry 2 Success 3 Fail 4 InProgress */
    unsigned char updating_now; /* 0x30  set while the worker runs this device  */
    unsigned char _pad31[0x07]; /* 0x31                                         */
} bg_device_entry;

/*
 * std::__detail::_Hash_node for the device map - the worker/log walk the
 * singly-linked bucket list rooted at BackgroundUpdate+0x38.
 *   node[0] = next, node[1..4] = std::string key (the device name),
 *   node[5] = DeviceEntry*, node[7] = cached hash.
 */
typedef struct bg_map_node {
    struct bg_map_node *next;   /* 0x00  _Hash_node_base::_M_nxt              */
    str_t               name;   /* 0x08  device-name key (data 0x08..)        */
    bg_device_entry    *entry;  /* 0x28  mapped value (node[5])               */
    unsigned long       hash;   /* 0x30  cached hash code (node[7] == +0x38)  */
} bg_map_node;

/*
 * std::unordered_map<string,DeviceEntry*> control block (BackgroundUpdate 0x28).
 *   0x00 buckets, 0x08 bucket count, 0x10 before-begin node, 0x18 element count,
 *   0x20 max-load-factor, 0x28 single-bucket, ...
 * Modelled opaque; the table primitives above operate on it.
 */
typedef struct bg_device_map {
    void          **buckets;       /* 0x00 (rel +0x28) */
    unsigned long   bucket_count;  /* 0x08 (rel +0x30) */
    bg_map_node    *before_begin;  /* 0x10 (rel +0x38) */
    unsigned long   element_count; /* 0x18 (rel +0x40) */
    unsigned char   _rest[0x20];   /* 0x20 rehash policy + single bucket */
} bg_device_map;

/*
 * std::deque<std::string> work queue (BackgroundUpdate 0x70..0xa8): map ptr,
 * start/finish iterators (cur/first/last/node) per the OEM block-deque layout.
 * Modelled opaque; the deque primitives operate on it.
 */
typedef struct bg_work_deque {
    str_t  *start_cur;    /* 0x70 (rel +0x70) front element cursor  */
    unsigned char _r0[0x10]; /* 0x78 start first/last */
    void  **start_node;   /* 0x88 start map node */
    str_t  *finish_cur;   /* 0x90 back cursor (one past) */
    unsigned char _r1[0x08]; /* 0x98 */
    void  **finish_last;  /* 0xa0 finish last */
    void  **finish_node;  /* 0xa8 finish map node */
} bg_work_deque;

/*
 * BackgroundUpdate - the whole object (offsets are OEM byte offsets).
 * Only the fields this TU touches are typed; the pthread mutex/condvar storage
 * is left as raw bytes so the named fields land on their real offsets.
 */
typedef struct BackgroundUpdate {
    unsigned char  _pad00[0x10];  /* 0x00  vptr + IUpdateClient base words      */
    void          *publisher;     /* 0x10  OD/MQTT publisher handle             */
    void          *factory;       /* 0x18  IUpdateClient factory (resolve +0x10)*/
    void          *registry;      /* 0x20  registered-client list (begin 0x28 / end 0x30) */
    bg_device_map  devices;       /* 0x28  unordered_map<string,DeviceEntry*>   */
    bg_work_deque  queue;         /* 0x70  deque<string> pending devices        */
    unsigned char  mutex[0x30];   /* 0xb0  pthread_mutex_t (table lock)         */
    unsigned char  cond[0x30];    /* 0xe0  std::condition_variable (worker wake) */
    unsigned char  running;       /* 0x118 worker run flag                      */
    unsigned char  _pad119[0x07]; /* 0x119                                      */
    void          *notify_cv;     /* 0x120 helper condvar object (FUN_00158840/00159080) */
    unsigned char  _pad128[0x04]; /* 0x128                                      */
    int            battery_charge;/* 0x12c second gate (>= 31 to run)           */
    unsigned short battery_soc;   /* 0x130 battery SoC gate (>= 11 to run)      */
    unsigned char  _pad132[0x06]; /* 0x132                                      */
} BackgroundUpdate;

/* ---- std::string model (the str_* set this TU uses, beyond update_common.h) ----
   Real type is libstdc++ std::__cxx11::string; modelled as a concrete buffer
   (str_t is defined in structs[]). */
void        str_init(str_t *s, const char *cstr);
void        str_free(str_t *s);
const char *str_cstr(const str_t *s);
void        str_assign_cstr(str_t *s, const char *cstr);
void        str_append_str(str_t *s, const str_t *src);
bool        str_equals_cstr(const str_t *s, const char *cstr);
/* std::string::find(token) + replace(token -> value) - OEM uses string::find
   for s_+_00170148 then string::_M_replace (FUN_00111f30 / remove_device). */
void        str_replace_token(str_t *s, const char *token, const str_t *value);

/* C++ operator new (OEM operator_new). Sizes used here: 0x38 (DeviceEntry),
   0x40 (SmpModemUpdateClient). */
void  *operator_new(size_t n);

/* ---- BackgroundUpdate table primitives (modelled std::unordered_map +
   std::deque + the pthread mutex/condvar at 0xb0 / 0xe0 / 0x120). ---- */
/* device map<string,DeviceEntry*> at 0x28..0x60 */
bg_device_entry  *bg_map_find(bg_device_map *m, const str_t *name);   /* FUN_00116550 (0 if absent) */
bg_device_entry **bg_map_emplace_slot(bg_device_map *m, const str_t *name); /* FUN_00117720 (insert/return slot) */
void              bg_map_erase(bg_device_map *m, const str_t *name); /* erase + free entry */

/* work deque<string> at 0x70..0xa8 */
bool  bg_deque_empty(bg_work_deque *q);
bool  bg_deque_nonempty(bg_work_deque *q);
void  bg_deque_push_back(bg_work_deque *q, const str_t *name);
void  bg_deque_pop_front(bg_work_deque *q, str_t *out_name);   /* move front -> out, pop */

/* table mutex (0xb0) + worker condvar (0xe0) + notify condvar (0x120). */
void  bg_table_lock(BackgroundUpdate *bg);
void  bg_table_unlock(BackgroundUpdate *bg);
void  bg_cond_wait(BackgroundUpdate *bg);          /* condition_variable::wait @0xe0 */
void  bg_cond_notify_all(BackgroundUpdate *bg);    /* notify_all @0xe0 */
void  bg_cond_prepare_wait(BackgroundUpdate *bg);  /* FUN_00158840 (cv @0x120 enter) */
void  bg_cond_after_pop(BackgroundUpdate *bg);     /* FUN_00159080 (cv @0x120 leave) */

/* settle helper (OEM nanosleep(secs,0) with EINTR retry). */
void  bg_sleep_seconds(int secs);

/* ---- OD / MQTT publisher (handle at 0x10, vtable-modelled) ---- */
/* publish a state/value string to a topic (vtable slot 0x20). */
void  bg_publisher_publish_state(void *pub, const str_t *topic, const str_t *value);
/* publish (retained) an empty message to clear a topic (vtable slot 0x30). */
void  bg_publisher_publish(void *pub, const str_t *topic, int retain);
/* read the device's current OD value into out (vtable slot 0x20 read path). */
void  bg_publisher_get_value(void *pub, const str_t *topic, bg_od_value *out);
void  bg_od_value_free(bg_od_value *v);

/* ---- IUpdateClient resolution (registry/factory at 0x18, + the special
   SmpModemUpdateClient for device "modem_nordic_stack" == DAT_001a1c50). ---- */
bool   bg_client_registry_contains(void *registry, const str_t *name);  /* registry @0x20+8/0x10 */
void  *bg_client_registry_resolve(void *factory, const str_t *name);    /* factory vtable slot 0x10 */
void   smp_modem_update_client_ctor(void *self, void *bg);   /* operator_new(0x40) + ctor */
/* IUpdateClient vtable: slot 0x10 = run update (returns result code, 0==ok);
   slot 0x08 = destroy. */
char   bg_update_client_run(void *client);
void   bg_update_client_destroy(void *client);
/* =========================================================== */

/*
 * background_update.c - VanMoof "update" OTA service
 *
 * Reconstruction of BackgroundUpdate: the per-device retry table + the
 * background worker thread that drains a queued device, resolves/constructs
 * its IUpdateClient, runs the update and republishes the per-device state over
 * MQTT.  Behaviour-oriented translation of the decompiled AArch64 image
 * (program "update", image base 0x100000).  Source path baked into the binary:
 *   devices/main/update/src/background_update.cpp
 *
 * The std::string / nlohmann::json / std::deque / std::unordered_map / the vm
 * OD-MQTT publisher / the IUpdateClient factory are MODELLED via update_common.h
 * plus the externs declared alongside this file - not rebuilt.  The VanMoof
 * algorithm (max-tries>=1 + dedup add, enqueue+notify, the battery-SoC gated
 * worker loop, the result-enum -> string map, terminal-result removal, and the
 * MQTT add/remove command parse) is translated verbatim, with all topics,
 * result strings, gate thresholds and source line numbers reproduced exactly.
 */

/* ------------------------------------------------------------------------- */
/* Result-state -> display-string map (OEM .rodata, used in 4 places).       */
/*   0 Idle  1 FailRetry  2 Success  3 Fail  4 InProgress  default Unknown    */
/* Reproduces the if/else ladder at e.g. 0x114f80 / log_device_states.       */
/* ------------------------------------------------------------------------- */
static const char *bg_result_string(unsigned state)
{
    switch (state) {
    case 0:  return "Idle";        /* s_Idle_00170038       */
    case 1:  return "FailRetry";   /* s_FailRetry_001700c0  */
    case 2:  return "Success";     /* s_Success_001700d0    */
    case 3:  return "Fail";        /* s_Fail_001700d8       */
    case 4:  return "InProgress";  /* s_InProgress_001700e0 */
    default: return "Unknown";     /* s_Unknown_001700f0    */
    }
}

/* ------------------------------------------------------------------------- */
/* DeviceEntry ctor - OEM FUN_00111f30.                                      */
/* Allocates+inits one table entry (0x38 bytes): OD/publisher handle at 0x00,*/
/* the per-device "update/background_update/progress_info/<name>" topic at   */
/* 0x08, remaining tries at 0x28, result state at 0x2c (Idle), "updating now"*/
/* flag at 0x30.  The "+" in the progress_info template is replaced with the */
/* device name.                                                              */
/* ------------------------------------------------------------------------- */
/* OEM 0x111f30 */
static void bg_device_entry_ctor(bg_device_entry *e, void *publisher,
                                 const str_t *name, int max_tries)
{
    e->publisher  = publisher;          /* 0x00 */
    str_init(&e->topic, "");            /* 0x08 (SSO at 0x18) */
    e->remaining_tries = max_tries;     /* 0x28 */
    e->result_state    = 0;             /* 0x2c = Idle */
    e->updating_now    = 0;             /* 0x30 */

    /* topic = "update/background_update/progress_info/+" with + -> name. */
    str_assign_cstr(&e->topic, "update/background_update/progress_info/+");
    str_replace_token(&e->topic, "+", name);   /* find "+" (s_+_00170148) */
}

/* ------------------------------------------------------------------------- */
/* set_in_progress - OEM FUN_00112260.                                       */
/* Moves the entry to state 4 (InProgress), publishes the state string over  */
/* the entry's progress_info topic (publisher vtable slot 0x20), and marks   */
/* "updating now" (0x30) true.                                               */
/* ------------------------------------------------------------------------- */
/* OEM 0x112260 */
static void bg_entry_set_in_progress(bg_device_entry *e)
{
    str_t s;
    str_init(&s, bg_result_string(4));      /* "InProgress" */
    bg_publisher_publish_state(e->publisher, &e->topic, &s);  /* vtbl+0x20 */
    str_free(&s);
    e->updating_now = 1;                     /* 0x30 <- 1 */
}

/* ------------------------------------------------------------------------- */
/* set_update_result - OEM FUN_001126e0.                                     */
/* Maps the flash result (0 == success) to the next entry state and          */
/* republishes it:                                                           */
/*   result == 0          -> state 2 (Success)                               */
/*   else, tries < 2      -> state 3 (Fail)        [give up]                 */
/*   else                 -> state 1 (FailRetry), remaining_tries--          */
/* ------------------------------------------------------------------------- */
/* OEM 0x1126e0 */
static void bg_entry_set_update_result(bg_device_entry *e, char result)
{
    str_t s;

    if (result == 0) {
        e->result_state = 2;                /* Success */
    } else if ((unsigned)e->remaining_tries < 2) {
        e->result_state = 3;                /* Fail */
    } else {
        e->result_state = 1;                /* FailRetry */
        e->remaining_tries = e->remaining_tries - 1;
    }

    str_init(&s, bg_result_string(e->result_state));
    bg_publisher_publish_state(e->publisher, &e->topic, &s);  /* vtbl+0x20 */
    str_free(&s);
}

/* ------------------------------------------------------------------------- */
/* log_device_states - OEM 0x1114c0.                                         */
/* Walks the device map's bucket-list (head at 0x38, _Hash_node `next` at 0) */
/* and logs every entry's name, "updating now" flag, last-result string and  */
/* remaining tries.                                                          */
/* ------------------------------------------------------------------------- */
/* OEM 0x1114c0 */
void background_update_log_device_states(BackgroundUpdate *bg)
{
    bg_map_node *n;

    for (n = bg->devices.before_begin; n != NULL; n = n->next) {
        bg_device_entry *e = n->entry;     /* node[5] */
        const char *result = bg_result_string(e->result_state);

        common_logf("devices/main/update/src/background_update.cpp", 0x71, LOG_INFO,
                    "Device name: %s - is updating now: %d - Last result: %s - "
                    "remainder tries: %d",
                    str_cstr(&n->name), e->updating_now != 0,
                    result, e->remaining_tries);
    }
}

/* ------------------------------------------------------------------------- */
/* add_device - OEM 0x113850.                                                */
/* Under the table mutex (0xb0): refuse max_tries==0, dedup against the map   */
/* (FUN_00116550), otherwise allocate a DeviceEntry and install it in the    */
/* unordered_map<string,DeviceEntry*> keyed by the device name.              */
/* ------------------------------------------------------------------------- */
/* OEM 0x113850 */
void background_update_add_device(BackgroundUpdate *bg, const str_t *name,
                                  int max_tries)
{
    bg_device_entry *e;
    bg_device_entry **slot;

    bg_table_lock(bg);                      /* mutex at 0xb0 */

    if (max_tries == 0) {
        common_logf("devices/main/update/src/background_update.cpp", 0x7b, LOG_WARN,
                    "Max tries can't be less than 1");
        bg_table_unlock(bg);
        return;
    }

    if (bg_map_find(&bg->devices, name) != NULL) {
        common_logf("devices/main/update/src/background_update.cpp", 0x85, LOG_WARN,
                    "%s is already added", str_cstr(name));
        bg_table_unlock(bg);
        return;
    }

    common_logf("devices/main/update/src/background_update.cpp", 0x81, LOG_INFO,
                "Add %s to background update table to be updated later when it "
                "is triggerd", str_cstr(name));

    e = (bg_device_entry *)operator_new(0x38);
    bg_device_entry_ctor(e, bg->publisher, name, max_tries);

    /* map[name] = e  (FUN_00117720 inserts/returns the bucket slot). */
    slot = bg_map_emplace_slot(&bg->devices, name);
    *slot = e;

    bg_table_unlock(bg);
}

/* ------------------------------------------------------------------------- */
/* trigger_update - OEM 0x1120e0.                                            */
/* Under the table mutex (0xb0): look the device up; only if its result      */
/* state is still < 2 (Idle/FailRetry, i.e. not already Success/Fail) push   */
/* the device name onto the work deque (0x70..) and notify the worker.       */
/* Returns 0 on success, -1 if the device isn't in the table.                */
/* ------------------------------------------------------------------------- */
/* OEM 0x1120e0 */
int background_update_trigger_update(BackgroundUpdate *bg, const str_t *name)
{
    bg_device_entry *e;
    int rc;

    bg_table_lock(bg);                      /* mutex at 0xb0 */

    e = bg_map_find(&bg->devices, name);
    if (e == NULL) {
        rc = -1;
    } else {
        rc = 0;
        if ((unsigned)e->result_state < 2) {
            bg_deque_push_back(&bg->queue, name);    /* 0x70.. deque */
            common_logf("devices/main/update/src/background_update.cpp", 0x92,
                        LOG_INFO,
                        "TriggerUpdate - Send %s to background update queue",
                        str_cstr(name));
            bg_cond_notify_all(bg);                  /* condvar at 0xe0 */
        }
    }

    bg_table_unlock(bg);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* remove_device - OEM 0x112c40.                                             */
/* Erases the device from the map (and releases its entry), then publishes a */
/* retained-clear on "update/background_update/<name>" and, after a 1s        */
/* settle, publishes the device's last-known OD value to                     */
/* "update/background_update/finished/<name>".                               */
/* ------------------------------------------------------------------------- */
/* OEM 0x112c40 */
void background_update_remove_device(BackgroundUpdate *bg, const str_t *name)
{
    str_t topic;
    str_t finished_topic;
    bg_od_value value;

    /* Erase from the unordered_map (decrements the entry refcount / frees). */
    bg_map_erase(&bg->devices, name);

    common_logf("devices/main/update/src/background_update.cpp", 0xd9, LOG_INFO,
                "Remove %s from background update table", str_cstr(name));

    /* topic = "update/background_update/+" with + -> name. */
    str_init(&topic, "update/background_update/+");      /* len 0x1a */
    str_replace_token(&topic, "+", name);                /* s_+_00170148 */

    /* Publish an empty retained message to clear the per-device state. */
    bg_publisher_publish(bg->publisher, &topic, 1);      /* vtbl+0x30, retain */

    /* Settle 1s (OEM nanosleep(1,0) w/ EINTR retry). */
    bg_sleep_seconds(1);

    /* Republish the device's current OD value on the "finished/" topic. */
    str_init(&finished_topic, "update/background_update/finished/");
    str_append_str(&finished_topic, name);
    bg_publisher_get_value(bg->publisher, &finished_topic, &value);  /* vtbl+0x20 */
    bg_od_value_free(&value);

    str_free(&finished_topic);
    str_free(&topic);
}

/* ------------------------------------------------------------------------- */
/* worker_loop - OEM 0x114ed0. The background update thread body.            */
/*                                                                           */
/* Runs while the `running` flag (0x118) is set. Each pass holds the table   */
/* mutex (0xb0) and gates on three conditions:                               */
/*   - the work deque (0x70..0xa8) is non-empty, AND                         */
/*   - battery SoC field at 0x130 >= 11 (0xb), AND                           */
/*   - a second SoC/charge gate at 0x12c >= 31 (0x1f).                       */
/* If any gate fails it waits on the condvar (0xe0) until they hold (or the  */
/* thread is asked to stop).                                                 */
/*                                                                           */
/* When work is available it pops the front device name, then resolves its   */
/* IUpdateClient:                                                            */
/*   - the special "modem_nordic_stack" device gets a freshly-constructed    */
/*     SmpModemUpdateClient (operator_new 0x40 + ctor, bg arg 0x10);         */
/*   - otherwise the device is looked up in the registered-client list       */
/*     (factory at 0x18, vtable slot 0x10 resolves a client by name).        */
/* It marks the entry InProgress, logs "Start update for %s", runs the       */
/* update (client vtable slot 0x10), logs success/failure, clears the entry  */
/* "updating now" flag, maps the result to the next state, logs the mapped   */
/* result string and - on a terminal Success/Fail (state 2 or 3) - removes   */
/* the device from the table.  The client is then destroyed (vtable slot 8). */
/* ------------------------------------------------------------------------- */
/* OEM 0x114ed0 */
void background_update_worker_loop(BackgroundUpdate *bg)
{
    while (bg->running) {                         /* 0x118 */
        bg_table_lock(bg);                        /* mutex at 0xb0 */

        /* Gate: queue non-empty AND SoC(0x130) >= 11 AND charge(0x12c) >= 31. */
        if (bg_deque_empty(&bg->queue) ||
            bg->battery_soc < 0xb ||              /* 0x130 < 11 */
            bg->battery_charge < 0x1f) {          /* 0x12c < 31 */
            /* Park the condvar (notify cv at 0x120) and re-check on wake. */
            bg_cond_prepare_wait(bg);
            if (bg->running) {
                while (!(bg_deque_nonempty(&bg->queue) &&
                         bg->battery_soc > 10 &&
                         bg->battery_charge > 0x1e)) {
                    bg_cond_wait(bg);             /* condvar at 0xe0 */
                    if (!bg->running)
                        break;
                }
            }
            bg_table_unlock(bg);
            continue;
        }

        /* Notify-on-pop bookkeeping (cv at 0x120), then pop the front name. */
        bg_cond_after_pop(bg);

        str_t name;
        bg_deque_pop_front(&bg->queue, &name);    /* 0x70.. front -> name */

        /* Resolve the IUpdateClient for this device. */
        void *client = NULL;
        bool have_client = false;

        if (str_equals_cstr(&name, "modem_nordic_stack")) {   /* DAT_001a1c50 */
            client = operator_new(0x40);
            smp_modem_update_client_ctor(client, bg->publisher); /* bg arg 0x10 */
            have_client = true;
        } else {
            /* Confirm the device is a registered client, then resolve it
               from the factory (0x18, vtable slot 0x10). */
            if (bg_client_registry_contains(bg->registry, &name)) {
                client = bg_client_registry_resolve(bg->factory, &name); /* +0x10 */
                have_client = (client != NULL);
            }
            if (!have_client) {
                common_logf("devices/main/update/src/background_update.cpp",
                            0xca, LOG_ERR, "%s update client doesn't exist ",
                            str_cstr(&name));
            }
        }

        if (have_client) {
            bg_device_entry *e;
            char result;
            unsigned state;

            /* Entry -> InProgress, then run the update. */
            e = *bg_map_emplace_slot(&bg->devices, &name);
            bg_entry_set_in_progress(e);

            common_logf("devices/main/update/src/background_update.cpp", 0xb5,
                        LOG_INFO, "Start update for %s", str_cstr(&name));

            result = bg_update_client_run(client);          /* vtable slot 0x10 */
            if (result == 0) {
                common_logf("devices/main/update/src/background_update.cpp", 0xb9,
                            LOG_INFO, "Successful Update for %s", str_cstr(&name));
            } else {
                common_logf("devices/main/update/src/background_update.cpp", 0xbb,
                            LOG_INFO, "Failed update for %s", str_cstr(&name));
            }

            /* Clear "updating now", then map result -> next entry state. */
            e = *bg_map_emplace_slot(&bg->devices, &name);
            e->updating_now = 0;                            /* 0x30 <- 0 */

            e = *bg_map_emplace_slot(&bg->devices, &name);
            bg_entry_set_update_result(e, result);

            e = *bg_map_emplace_slot(&bg->devices, &name);
            state = (unsigned)e->result_state;              /* 0x2c */

            common_logf("devices/main/update/src/background_update.cpp", 0xc2,
                        LOG_INFO, "Update result for %s is: %s ",
                        str_cstr(&name), bg_result_string(state));

            /* Terminal Success(2) or Fail(3) -> drop the device. */
            if ((unsigned)(state - 2) < 2) {
                str_t dev;
                str_init(&dev, str_cstr(&name));
                background_update_remove_device(bg, &dev);
                str_free(&dev);
            }

            /* Destroy the client (vtable slot 8). */
            bg_update_client_destroy(client);
        }

        str_free(&name);
        bg_table_unlock(bg);
    }
}

/* ------------------------------------------------------------------------- */
/* on_mqtt_message - OEM 0x113c10.                                           */
/* Topic is "update/background_update/<device>/<cmd>": the device name is the */
/* path component at index 2 (split on '/'). Then parse the JSON payload:     */
/*   - "maxTries" (int, default 10) and "add" (bool): when add==true call     */
/*     add_device(<device>, maxTries).                                        */
/*   - "trigger" (bool): when true call trigger_update(<device>).             */
/* JSON access throws if the payload root isn't an object; the OEM emits the  */
/* nlohmann "cannot use operator[] with a string argument" diagnostic and a   */
/* "Could not device name from the topic, %s" log on a malformed topic.       */
/* ------------------------------------------------------------------------- */
/* OEM 0x113c10 */
void background_update_on_mqtt_message(BackgroundUpdate **self, const str_t *topic,
                                       const json_t *payload)
{
    BackgroundUpdate *bg = *self;
    char parts[8][64];
    size_t nparts;
    str_t device;
    int max_tries;
    bool add;
    bool trigger;

    /* Split the topic on '/'; component [2] is the device name. */
    nparts = str_split(str_cstr(topic), "/", parts, 8);
    if (nparts < 3) {
        common_logf("devices/main/update/src/background_update.cpp", 0x46, LOG_WARN,
                    "Could not device name from the topic, %s", str_cstr(topic));
        return;
    }
    str_init(&device, parts[2]);

    /* payload["maxTries"] (default 10) + payload["add"]. */
    max_tries = 10;
    json_get_int(payload, "maxTries", &max_tries);

    add = false;
    if (json_get_bool(payload, "add", &add) && add) {
        background_update_add_device(bg, &device, max_tries);
    }

    /* payload["trigger"]. */
    trigger = false;
    if (json_get_bool(payload, "trigger", &trigger) && trigger) {
        background_update_trigger_update(bg, &device);
    }

    str_free(&device);
}
