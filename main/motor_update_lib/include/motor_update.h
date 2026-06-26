/*
 * motor_update.h — model for the VanMoof S5 `motor_update_example` CLI and the
 * C2000 SCI flasher library (pkg vmxs5-embedded-motor-update-lib).
 *
 * `/usr/bin/motor_update_example` flashes the TI **TMS320F280049C** motor
 * controller over the **SCI** serial bootloader: set boot mode → autobaud →
 * upload the SCI **flash kernel** (`tms320f40049c_sci_kernel.bin`, a TI blob)
 * into DSP RAM → autobaud → have the kernel program the **application** image →
 * restore boot mode. Program "motor_update_example", AArch64, image base
 * 0x100000.
 *
 * The flasher (boot-mode GPIO, serial setup, the SCI upload + flash protocol) is
 * the VanMoof "motor-update-lib"; reconstructed in src/c2000_flash.c. The TI SCI
 * wire format and libstdc++/libc are vendor (modelled). The CLI driver is
 * src/main.c. OEM addresses are quoted per function.
 */
#ifndef MOTOR_UPDATE_H
#define MOTOR_UPDATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- opaque handles ---------------------------------------------------- */
typedef struct c2000_serial  c2000_serial;   /* serial_port.cpp wrapper (termios) */
typedef struct c2000_flasher c2000_flasher;  /* the SCI flasher object            */
typedef struct c2000_image   c2000_image;    /* a firmware image read from a .bin  */

/* progress callback invoked during an upload/flash (modelled). */
typedef void (*c2000_progress_cb)(void *ctx, size_t done, size_t total);

/* The OEM throws std::runtime_error on protocol failure (caught by main as
 * "exception caught: <what>"). Modelled here as a 0/-1 return + this accessor
 * returning the message of the last failure (the verbatim OEM throw string). */
const char    *c2000_last_error(void);

/* ---- boot-mode GPIO (board-specific) ----------------------------------- *
 * Drive the motor controller's boot-mode lines (sysfs GPIO): pin 12 is a strobe
 * pulsed high across the change then dropped; pin 125 is held 1; pin 126 carries
 * the mode (0 = enter SCI boot, 1 = run the application). 20 ms settle. mode
 * outside [0,1] is the OEM throw "Unsupported C2000 Boot Mode". (OEM 0x1071c0.) */
void           c2000_set_boot_mode(int mode);                        /* 0x1071c0 */

/* ---- serial + flasher -------------------------------------------------- */
c2000_serial  *c2000_serial_open(const char *ttydev);                /* 0x109b90 */
void           c2000_serial_close(c2000_serial *s);
c2000_flasher *c2000_flasher_new(c2000_serial *s);                   /* 0x107820 */
void           c2000_flasher_delete(c2000_flasher *f);

/* TI C2000 SCI autobaud lock ("Determine baud rate..."): up to 50 tries of
 * {flush, send 'A', read echo}; -1 + "baud rate detection failed" on timeout. */
int            c2000_autobaud(c2000_flasher *f);                     /* 0x107e60 */

/* ---- firmware images --------------------------------------------------- *
 * Open a .bin into a byte buffer (OEM: std::ifstream binary). c2000_image_ok()
 * is the stream-good test ((state & 5) == 0); a bad stream is the OEM throw
 * "unable to open kernel/application image". */
c2000_image   *c2000_image_open(const char *path);                   /* 0x105f20 */
bool           c2000_image_ok(const c2000_image *img);
void           c2000_image_close(c2000_image *img);

/* Stage 1: byte-by-byte echo upload of the SCI flash kernel to the DSP ROM
 * bootloader (each sent byte is echoed back and verified). -1 + "failed". */
int            c2000_send_kernel(c2000_flasher *f, c2000_image *kernel,
                                 c2000_progress_cb cb, void *ctx);    /* 0x107850 */
/* Stage 2: the running flash kernel programs the application via the DFU block
 * protocol (start packet, checksummed blocks, status). -1 + "Flashing failed" /
 * "Invalid checksum" / "Invalid start header" / "Failed sending DFU packet". */
int            c2000_flash_app(c2000_flasher *f, c2000_image *app);  /* 0x107ae0 */

#endif /* MOTOR_UPDATE_H */
