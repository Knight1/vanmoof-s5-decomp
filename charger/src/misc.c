/*
 * misc.c — charger fault reporting, fault-code lookup, version id.
 *
 * VanMoof S5 Liteon charger (model 5EL00000000EB). Small charger-app helpers:
 * the fault-event reporter, the fault-code table lookup, and the device version
 * id accessor. Translated from the OEM image (raw ARM Cortex-M, base 0x0). The
 * event-post is vendor; the fault control register and the off-image fault-code
 * table are satisfied at link.
 */

#include "charger.h"

/* charger_report_fault_event — raise a charge fault.
 *
 * OEM disassembly (0x000016c0..0x000016d2):
 *
 * Writes 0x500 to the fault control register (peripheral +0x58) and posts the
 * fault event (id 0, code 0x16, arg 0).
 */
void charger_report_fault_event(void)
{
    *(volatile uint32_t *)(CHG_FAULT_PERIPH + 0x58) = 0x500;
    charge_event_post(0, 0x16, 0);
}

/* charger_find_fault_index — find a code in the 8-entry fault-code table.
 *
 * OEM disassembly (0x00003e88..0x00003e9e):
 *
 * Linear scan; returns the matching index, or 8 if the code is not present. The
 * table is a flash constant beyond this image slice (CHG_FAULT_TABLE = 0x80b0).
 */
int charger_find_fault_index(int code)
{
    const int32_t *table = (const int32_t *)CHG_FAULT_TABLE;
    int i = 0;

    do {
        if (table[i] == code)
            return i;
        i++;
    } while (i != 8);
    return i;
}

/* charger_get_version_id — return the device/version id.
 *
 * OEM disassembly (0x00005070..0x00005074):
 */
int charger_get_version_id(void)
{
    return CHG_VERSION_ID;
}

/* charger_flash_program_record — bounded erase + write of the 0x200-byte page.
 *
 * OEM disassembly (0x00001d74..0x00001daa):
 *
 * Refuses (returns 0) if (addr + count) reaches 0x10000. Otherwise erases the
 * 0x200-byte page at 0xfe00 and, on success, writes `buf` (0x200 bytes) to
 * `addr`, returning 1. Shared bounded flash-page programmer (tail-called from
 * charger_write_identity_config_record); the erase/write primitives are vendor.
 * Note the erase address is the hard-coded 0xfe00 while the write uses `addr`.
 */
static int charger_flash_program_record(uint32_t addr, uint32_t count, void *buf)
{
    if (addr + count >= 0x10000u)
        return 0;
    if (charge_flash_erase((void *)CHG_FLASH_CTX, 0xfe00, 0x200, CHG_FLASH_KEY) != 0)
        return 0;
    charge_flash_write((void *)CHG_FLASH_CTX, addr, buf, 0x200);
    return 1;
}

/* charger_write_identity_config_record — build + persist the identity records.
 *
 * OEM disassembly (0x00003024..0x00003076):
 *
 * Builds two records in a RAM buffer: record 1 (header 0x13) over REC1 and record
 * 2 (header 0x11) over REC2, each protected by a subtractive checksum stored at
 * its byte 2. Then tail-calls charger_flash_program_record(0xfe00, 0x24, rec) to
 * persist the 0x200-byte page. Returns 1 on success, 0 on failure.
 */
int charger_write_identity_config_record(void)
{
    char *rec = (char *)CHG_IDENT_REC1;
    const char *p;
    char ck;
    short n;

    rec[0] = 0x13;
    rec[1] = 0;
    rec[2] = (char)-1;
    ck = 0;
    p = rec;
    n = 0x14;
    while (n = (short)(n - 1), n != 0) {
        ck = (char)(ck - *p);
        p++;
    }
    rec[0x14] = 0x11;
    rec[0x15] = 0;
    rec[0x16] = (char)-1;
    rec[2] = ck;

    ck = 0;
    p = (const char *)CHG_IDENT_REC2;
    n = 0x12;
    while (n = (short)(n - 1), n != 0) {
        ck = (char)(ck - *p);
        p++;
    }
    rec[0x16] = ck;

    return charger_flash_program_record(0xfe00, 0x24, rec);
}

/* charger_process_command_record — dispatch a received command record.
 *
 * OEM disassembly (0x00005424..0x00005474):
 *
 * If neither low flag bit (record +0xc) is set, rejects (-1). Otherwise, unless
 * bit 3 is set, runs the transfer: sets up the transfer, sends the +0x14 buffer,
 * receives into the +0x10 buffer when bit 11 is set, and on the command tag
 * 0xad8 (bits 31:22) copies + finishes the 0x20-byte +0x20 payload. Then zeroes
 * the 0x3c-byte record and returns 0. (The transfer helpers are vendor.)
 */
int charger_process_command_record(void *rec)
{
    uint8_t  *r = (uint8_t *)rec;
    uint32_t  flags = *(uint32_t *)(r + 0xc);

    if ((flags & 3) == 0)
        return -1;

    if ((int32_t)(flags << 0x1c) >= 0) {
        void *recv_buf = *(void **)(r + 0x10);
        void *send_buf = *(void **)(r + 0x14);

        charge_cmd_xfer_setup();
        charge_cmd_send(send_buf);
        if ((int32_t)(flags << 0x14) < 0)
            charge_cmd_recv(recv_buf);
        if ((flags & 0xffc00000) == 0xad800000) {
            uint8_t buf[36];
            charge_cmd_copy(buf, *(void **)(r + 0x20), 0x20);
            charge_cmd_finish(buf);
        }
    }

    mem_set(r, 0, 0x3c);
    return 0;
}
