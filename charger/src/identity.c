/*
 * identity.c — charger identity/version flash block read + write.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). The identity block lives in
 * flash; write_identity_block stamps a CRC-protected copy of the 0xfc00 page,
 * read_identity_version recomputes the CRC over the firmware image for an
 * integrity self-check. Translated from the OEM image (raw ARM Cortex-M,
 * base 0x0). The flash driver and CRC engine are vendor; the source pages
 * (0x9200 / 0xfc00 / 0x2600) extend beyond this image slice and are satisfied
 * at link. Disassembly-exact (footer byte order from the strb offsets).
 */

#include "charger.h"

/* charger_write_identity_block — stamp the identity page with a fresh CRC.
 *
 * OEM disassembly (0x000039dc..0x00003a66):
 *
 * Erases/preps the 0x9200 region; for mode 1 computes the CRC over the 0x6bf8-byte
 * image and sets the identity flag. Copies the 512-byte 0xfc00 page into a buffer,
 * overwrites the trailing 8 bytes — CRC big-endian at +0x1f8..+0x1fb, the mode at
 * +0x1fc, zeros at +0x1fd..+0x1ff — then writes the page back to 0xfc00.
 */
void charger_write_identity_block(int mode)
{
    uint8_t        buf[516]; /* sp - 0x204 */
    const uint8_t *src;
    uint8_t       *dst;
    uint32_t       crc = 0;

    if (charge_flash_op(0x9200, 0x36) != 0) {
        charge_error_post(0x20);
        return;
    }
    if (mode == 1) {
        crc = charge_crc(0x9200, 0x6bf8);
        *(volatile uint8_t *)CHG_IDENT_FLAG = 1;
    }

    src = (const uint8_t *)0xfc00;
    dst = buf;
    do {
        *dst++ = *src++;
    } while (src != (const uint8_t *)0xfe00);

    buf[0x1ff] = 0;
    buf[0x1fe] = 0;
    buf[0x1fd] = 0;
    buf[0x1fb] = (uint8_t)crc;
    buf[0x1fa] = (uint8_t)(crc >> 8);
    buf[0x1f9] = (uint8_t)(crc >> 16);
    buf[0x1f8] = (uint8_t)(crc >> 24);
    buf[0x1fc] = (uint8_t)mode;

    charge_flash_prep(0xfc00);
    charge_flash_write2(0xfc00, buf);
}

/* charger_read_identity_version — CRC the firmware image for an integrity check.
 *
 * OEM disassembly (0x00005166..0x000051c4):
 *
 * Selects the source page (0x2600 for mode 1, else 0x9200), preps it, then runs
 * the CRC engine over the 0x6bf8-byte image followed by an 8-byte 0xff footer.
 * Returns (and caches at CHG_IDENT_RESULT) the CRC result read from the context
 * at +8. Returns -1 if the flash prep fails.
 */
int charger_read_identity_version(int mode)
{
    const void *src = (mode == 1) ? (const void *)0x2600 : (const void *)0x9200;
    uint8_t     footer[8];
    void       *ctx;
    uint32_t    result;

    if (charge_flash_op((uint32_t)(uintptr_t)src, 0x36) != 0) {
        charge_error_post(0x20);
        return -1;
    }

    *(uint32_t *)&footer[0] = 0xffffffffu;
    *(uint32_t *)&footer[4] = 0xffffffffu;

    ctx = *(void **)CHG_CRC_CTX_PTR;
    charge_crc_init(ctx);
    charge_crc_update(ctx, (void *)(uintptr_t)src, 0x6bf8);
    charge_crc_update(ctx, footer, 8);

    result = *(uint32_t *)((uint8_t *)(*(void **)CHG_CRC_CTX_PTR) + 8);
    *(volatile uint32_t *)CHG_IDENT_RESULT = result;
    return (int)result;
}
