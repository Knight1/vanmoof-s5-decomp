/*
 * main.c — VanMoof S5 `motor_update_example`: TI C2000 (TMS320F280049C) motor
 * controller SCI firmware flasher.  OEM: utils (motor_update_example), program
 * "motor_update_example", AArch64, image base 0x100000, main @ 0x1062d0.
 *
 * Flashes the motor controller over its SCI serial bootloader in two stages:
 *   1. drive the boot-mode line into SCI-boot, autobaud-lock the ROM bootloader,
 *      and upload the SCI **flash kernel** (tms320f40049c_sci_kernel.bin) into
 *      DSP RAM;
 *   2. autobaud-lock the running kernel and have it program the **application**
 *      image into flash; then restore the boot-mode line to run.
 *
 * Behaviour-oriented C: the flasher library (boot-mode GPIO, serial, the SCI
 * upload + flash protocol) is modelled as the externs in motor_update.h and
 * reconstructed in c2000_flash.c; the TI SCI wire format and libstdc++ are
 * vendor. The CLI letters, defaults, console strings, and return codes are
 * reproduced verbatim from the 0x1062d0 decompilation.
 */
#include "motor_update.h"

#include <stdio.h>
#include <getopt.h>

/* OEM throws std::runtime_error on any failure; the top-level catch prints
 * "exception caught: <what>" and returns -1. Modelled here as this helper. */
static int flash_caught(const char *what)
{
    printf("exception caught: %s\n", what);
    return -1;
}

static void print_usage(const char *argv0)
{
    /* NB the usage text prints "[-g]" but the implemented flag is -b. */
    printf("Usage: %s [-g] [-p <ttydev>] <kernel file to send> <app file to send>\n", argv0);
    printf(" -b: skip setting boot mode (can come in handy for ubuntu x devboard)\n");
    printf(" -p: ttydev used for updating (defaults to /dev/ttymxc3)\n");
}

/*
 * run_flash — the flashing sequence (the body of main's try-block). Returns 0 on
 * success, -1 on a caught failure.
 */
static int run_flash(const char *ttydev, const char *kernel_path,
                     const char *app_path, bool skip_boot_mode)
{
    c2000_serial  *serial = c2000_serial_open(ttydev);
    c2000_flasher *fl     = c2000_flasher_new(serial);
    c2000_image   *kernel;
    c2000_image   *app;
    int rc = 0;

    /* enter SCI boot mode unless -b */
    if (!skip_boot_mode)
        c2000_set_boot_mode(0);

    /* ---- stage 1: upload the SCI flash kernel ---- */
    kernel = c2000_image_open(kernel_path);
    if (!c2000_image_ok(kernel)) {
        rc = flash_caught("unable to open kernel image");
        goto out_kernel;
    }

    printf("Determine baud rate... ");
    fflush(stdout);
    if (c2000_autobaud(fl) != 0) { rc = flash_caught(c2000_last_error()); goto out_kernel; }
    printf("Done!\n");

    printf("Sending flash kernel... ");
    fflush(stdout);
    if (c2000_send_kernel(fl, kernel, NULL, NULL) != 0) {
        rc = flash_caught(c2000_last_error());
        goto out_kernel;
    }
    printf("\rSending flash kernel... Done!\n");

    /* ---- stage 2: program the application via the kernel ---- */
    app = c2000_image_open(app_path);
    if (!c2000_image_ok(app)) {
        rc = flash_caught("unable to open application image");
        goto out_app;
    }

    printf("Determine baud rate... ");
    fflush(stdout);
    if (c2000_autobaud(fl) != 0) { rc = flash_caught(c2000_last_error()); goto out_app; }
    printf("Done!\n");

    printf("Flashing application... ");
    fflush(stdout);
    if (c2000_flash_app(fl, app) != 0) { rc = flash_caught(c2000_last_error()); goto out_app; }
    printf("\rFlashing application... Done!\n");

    /* return the controller to run mode unless -b */
    if (!skip_boot_mode)
        c2000_set_boot_mode(1);

out_app:
    c2000_image_close(app);
out_kernel:
    c2000_image_close(kernel);
    c2000_flasher_delete(fl);
    c2000_serial_close(serial);
    return rc;
}

int main(int argc, char **argv)
{
    const char *ttydev = "/dev/ttymxc3";   /* -p default */
    bool  skip_boot_mode = false;          /* -b */
    int   c;

    /* OEM option string is "bh:p:": -b skip boot mode, -p <ttydev>; anything
     * else falls through to the usage error. */
    while ((c = getopt(argc, argv, "bh:p:")) != -1) {
        switch (c) {
        case 'b':
            skip_boot_mode = true;
            break;
        case 'p':
            ttydev = optarg;
            printf("Serial port set to %s\n", ttydev);
            break;
        default:
            print_usage(argv[0]);
            return -22;                     /* -0x16 (EINVAL) */
        }
    }

    /* exactly two positionals: <kernel file> <app file> */
    if (optind + 2 != argc) {
        print_usage(argv[0]);
        return -22;
    }

    return run_flash(ttydev, argv[optind], argv[optind + 1], skip_boot_mode);
}
