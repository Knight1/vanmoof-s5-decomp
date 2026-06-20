/*
 * storage.c — VanMoof power_control flash/storage page writer.
 *
 * Translated from OEM ARM Cortex-M4F firmware
 *   power_control.20240129.145222.1.5.0.main.v1.5.0-main.bin   (image base 0x0)
 *
 * Functions:
 *   storage_erase_program_sectors   @ 0x000065b8
 */

#include "power_control.h"

/*
 * storage_erase_program_sectors — erase a run of 0x200-byte program sectors and
 * stage a write window for the storage device. // 0x000065b8
 *
 * OEM disassembly (0x000065b8..0x00006648):
 *
 * Clears the context session fields (+0x10 / +0x15), fetches the flash-bank
 * object from ctx+0x18 and bails with -1 if it is absent or its ready flag
 * (+0x3c) is clear. req[0] (u16) is the start sector id (must be < 0x3a, else
 * -1); req[1] (u16) is the sector count. After flushing any pending page
 * (storage_dev_prepare), and clamping the run so start+count stays within 0x3a,
 * it erases each 0x200-byte page from offset (start+0x40)*0x200 via
 * storage_dev_erase_sector; any erase error returns -1. It then detaches the
 * bank page (+0x44 = 0xffff), fills the page RAM buffer (+0x40) with 0xff over
 * 0x200 bytes, and arms the page id / remaining-count fields (+0x44 / +0x46).
 * Finally it records the accepted start at ctx+6 (clearing ctx+8) and returns 0.
 */
int storage_erase_program_sectors(int ctx, uint32_t unused2, const uint16_t *req, uint32_t unused4)
{
    int dev = *(int *)(ctx + 0x18);
    uint16_t id16;
    uint32_t id, count;
    uint32_t page_addr;
    uint16_t done;

    (void)unused2;
    (void)unused4;

    *(uint32_t *)(ctx + 0x10) = 0;
    *(uint8_t *)(ctx + 0x15) = 0;
    if (dev == 0 || *(char *)(dev + 0x3c) == 0) {
        return -1;
    }

    id16 = req[0];
    id = id16;
    if (id >= 0x3a) {
        return -1;
    }

    count = req[1];
    if (storage_dev_prepare(dev) != 0) {
        return -1;
    }

    if (count != 0) {
        done = 0;
        if (id + count > 0x39) {
            count = (0x3au - id) & 0xffff;
        }
        page_addr = (id + 0x40) * 0x200;
        do {
            if (storage_dev_erase_sector(dev, page_addr) != 0) {
                return -1;
            }
            done++;
            page_addr += 0x200;
        } while (done < count);
        *(uint32_t *)(dev + 0x44) = 0xffff;
        mem_set(*(void **)(dev + 0x40), 0xff, 0x200);
        *(uint16_t *)(dev + 0x44) = id16;
        *(uint16_t *)(dev + 0x46) = (uint16_t)count;
    }

    *(uint16_t *)(ctx + 6) = id16;
    *(uint16_t *)(ctx + 8) = 0;
    return 0;
}
