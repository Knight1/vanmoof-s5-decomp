/*
 * main.c — VanMoof S5 i.MX8 `lightweight_update` CLI flasher.
 *   OEM: utils/lightweight_update/main.cpp, program "lightweight_update",
 *   AArch64, image base 0x100000, main @ 0x108dc0.
 *
 * A standalone, trimmed update path: flash ONE firmware file to ONE device over
 * CAN (or the serial tty), without the `update` service's manifest walk / FOTA
 * orchestration / systemd-notify. It brings up the same vm/CAN context, MQTT
 * client and UpdateClientFactory the `update` service uses, asks the factory for
 * an IUpdateClient for the file, runs PerformUpdate(), and reports the result +
 * elapsed time.
 *
 * Behaviour-oriented C (the C++ update machinery — vm, IMQTTClient,
 * VersionClientMqtt, UpdateClientFactory, the IUpdateClient subclasses — is the
 * shared `update` code, modelled here as opaque externs; see ../update/). The
 * VanMoof control flow, option letters, defaults, and every console string are
 * reproduced verbatim from the decompilation.
 */
#include "lightweight_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

/* Signals trapped during a flash (DAT_0014b1a8: {2,3,15,11}). */
static const int k_trapped_signals[] = { SIGINT, SIGQUIT, SIGTERM, SIGSEGV };

/* MQTT identity + broker. The OEM ctor (FUN_00134db0) takes a 6th literal arg
 * 5000 (DAT_0014b1a0) that is stored in a client field (+0x58) — it is NOT the
 * port: the mosquitto connect is hardcoded to localhost:1883 (0x75b) keepalive
 * 60 (0x3c), matching /etc/mosquitto/mosquitto.conf 'listener 1883'. */
#define LWU_CLIENT_ID  "lightweight_update"
#define LWU_USER       "update-service"
#define LWU_HOST       "localhost"
#define LWU_PORT       1883

int g_lwu_verbose;            /* PTR_DAT_0016ef40 */

/* clean-flash signal handler (FUN_0010ab20) — modelled as a no-op trap. */
static void lwu_signal_handler(int sig) { (void)sig; }

static void lwu_print_usage(const char *argv0)
{
    printf("Usage: %s [-fv] [-b <can_channel>] [-t <target>] <file_to_send>\n", argv0);
    printf(" -b <can_bus>: the CAN bus to use, vcan0 by default\n");
    printf(" -f: force update even if the firmware version already matches\n");
    printf(" -p: ttydev used for updating (defaults to /dev/ttymxc3)\n");
    printf(" -t <target_name_or_address>: the device name or address to update."
           " Optional, if not provided, the device name is derived from the filename\n");
    printf(" -v: enable verbose logging\n");
    printf(" <file_to_send> - the name needs to include device name (unless overridden"
           " with -t) (and version), e.g. 'device_name.date.time.major.minor.patch.bin'"
           " or 'device_name.bin'\n");
}

/*
 * Derive the device name from the file path when no -t target is given: take the
 * basename (after the last '/') up to the first '.'. e.g.
 * "/opt/devices_fw/motor_control.2024….bin" -> "motor_control".
 * (OEM: rfind('/') @0x14b088 then find('.') @0x14b120.)
 */
static void derive_device_from_filename(const char *file, char *out, size_t out_sz)
{
    const char *base = strrchr(file, '/');
    const char *dot;
    size_t n;

    base = base ? base + 1 : file;
    dot  = strchr(base, '.');
    n    = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

int main(int argc, char **argv)
{
    const char *can_bus = "vcan0";          /* -b (default) */
    const char *ttydev  = "/dev/ttymxc3";   /* -p (default) */
    bool  force   = false;                  /* -f */
    bool  have_target = false;              /* -t given */
    uint8_t target_addr = 0;                /* -t numeric address */
    int   c;

    /* ---- CLI: getopt "vb:hft:p:" -------------------------------------- */
    while ((c = getopt(argc, argv, "vb:hft:p:")) != -1) {
        switch (c) {
        case 'b':                           /* CAN bus */
            can_bus = optarg;
            break;
        case 'f':                           /* force even if version matches */
            force = true;
            break;
        case 'v':                           /* verbose */
            g_lwu_verbose = 1;
            break;
        case 't': {                         /* target name OR numeric address */
            /* "0x" prefix -> base 16, else base 10 (OEM compares first ≤2 chars
             * to "0x" @0x14adb0; strtol via the "stoi" wrapper). */
            int base = (strncmp(optarg, "0x", 2) == 0) ? 16 : 10;
            target_addr = (uint8_t)strtol(optarg, NULL, base);
            have_target = true;
            break;
        }
        case 'p':                           /* serial tty for updating */
            ttydev = optarg;
            printf("Serial port set to %s\n", ttydev);
            break;
        case 'h':
        case '?':
        default:
            lwu_print_usage(argv[0]);
            return 1;
        }
    }

    /* exactly one positional <file_to_send> is required (the OEM also accepts a
     * two-positional form; the documented invocation is a single file path). */
    if (optind + 1 != argc) {
        lwu_print_usage(argv[0]);
        return 1;
    }
    const char *file_to_send = argv[optind];

    /* ---- bring up the channel ----------------------------------------- */
    for (size_t i = 0; i < sizeof k_trapped_signals / sizeof k_trapped_signals[0]; i++)
        signal(k_trapped_signals[i], lwu_signal_handler);

    vm_ctx *vm = vm_ctx_alloc_init();
    if (vm == NULL || vm_ctx_open_can(vm, can_bus) != 0) {
        vm_ctx_free(vm);
        printf("Could not init channel\n");
        return 1;
    }

    /* ---- update plumbing (shared `update` machinery) ------------------ */
    version_client *vc   = version_client_mqtt_new();
    mqtt_client    *mqtt = mqtt_client_new(LWU_CLIENT_ID, LWU_USER, 1, LWU_HOST, LWU_PORT);
    update_client_factory *factory = update_client_factory_new(vm, vc, mqtt, force);

    /* ---- resolve the target device name ------------------------------- */
    char device[64];
    if (have_target) {
        const char *name = target_addr_to_device_name(target_addr);
        if (strcmp(name, "invalid enum value") == 0) {
            printf("Error, update file '%s' for device '%u', cannot be processed\n",
                   file_to_send, target_addr);
            update_client_factory_delete(factory);
            mqtt_client_delete(mqtt);
            version_client_delete(vc);
            vm_ctx_free(vm);
            return 1;
        }
        snprintf(device, sizeof device, "%s", name);
    } else {
        derive_device_from_filename(file_to_send, device, sizeof device);
    }

    /* ---- run the update ----------------------------------------------- */
    long t_start = clock_ns();
    iupdate_client *client = factory_get_update_client(factory, file_to_send,
                                                       device, force);
    int rc;
    if (client == NULL) {
        printf("could not get an update client for: %s\n", file_to_send);
        rc = 1;
    } else if (iupdate_client_perform_update(client) != 0) {
        long ms = (clock_ns() - t_start) / 1000000;
        printf("Update was not performed.\n");
        printf("Spent time:%ld ms\n", ms);
        rc = 1;
    } else {
        long ms = (clock_ns() - t_start) / 1000000;
        printf("Update performed.\n");
        printf("Update successful.\n");
        printf("Spent time:%ld ms\n", ms);
        rc = 0;
    }

    /* ---- teardown ----------------------------------------------------- */
    if (client != NULL)
        iupdate_client_delete(client);
    update_client_factory_delete(factory);
    mqtt_client_delete(mqtt);
    version_client_delete(vc);
    vm_ctx_free(vm);
    return rc;
}
