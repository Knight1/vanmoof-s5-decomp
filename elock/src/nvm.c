/*
 * nvm.c — persisted lock-secret / AES-key management (elock).
 *
 * VanMoof S5/A5 electronic frame-lock controller. The NVM page driver
 * (nvm_record_*) and the AES block-cipher primitive are vendor; the glue that
 * reads/writes the 14-byte secret (record 0x30) and the 16-byte AES key
 * (record 0x40) and publishes them into the CAN Object-Dictionary (descriptors
 * 0x87xx / 0x91xx) is VanMoof. Translated from the OEM image (LPC546xx, base 0x0).
 */

#include "elock.h"

/*
 * elock_load_secret_to_od_87 — read the persisted lock secret from NVM and
 * publish its body into the comms Object-Dictionary descriptor 0x87xx.
 *
 * OEM disassembly (0x0000685a..0x00006958):
 *
 * Opens the persisted-record device proxy (nvm_record_open) and clears a 14-byte
 * scratch buffer. If the proxy opened, it device-reads record 0x30 (length 14)
 * into the scratch (nvm_record_read); the read result is normalised to 0 on
 * success / -1 on failure (rd_err). The first scratch byte is a tag/length
 * sentinel: if it is greater than 1 the record is treated as invalid — the
 * buffer is re-zeroed and rd_err forced to -1.
 *
 * The OD key is assembled from the caller context: ctx->ctx_id (ctx[0]) and a
 * one-byte OD sub-index taken from ctx[1] (byte at +4), combined with the
 * constant high byte 0x87 to form the descriptor selector {sub, 0x87, 0}. The
 * entry is looked up; if found, its slot lock is taken (timeout 100). With the
 * lock held the 13-byte secret body (scratch+1 .. scratch+13, i.e. everything
 * after the tag byte) is copied into the entry's data buffer (entry[0]). The
 * entry is re-looked-up with the same selector and released; publish_ok stays 0
 * on success, otherwise -1.
 *
 * Results are folded: rd_err (signature/read), publish_ok (OD publish) and the
 * proxy-close status collapse to a single 0/-1 return. Finally, if the caller
 * passed a non-NULL out pointer, the 14-byte scratch is verified against it
 * (mem_compare returns 0 when equal) and that result is merged into the return.
 */
int elock_load_secret_to_od_87(uint32_t *ctx, int out)
{
    int proxy;
    int rd_err;
    int publish_ok;
    uint8_t od_sel[4];
    uint8_t scratch[14];
    uint32_t **entry;
    uint32_t ctx_id;
    uint8_t sub;

    proxy = nvm_record_open();
    mem_set(scratch, 0, 0xe);
    if (proxy == 0) {
        rd_err = -1;
    } else {
        rd_err = nvm_record_read(proxy, scratch, 0x30, 0xe);
        rd_err = -(int)(rd_err != 0);
    }
    if (scratch[0] > 1) {
        mem_set(scratch, 0, 0xe);
        rd_err = -1;
    }

    sub    = *(uint8_t *)((uint8_t *)ctx + 4);
    ctx_id = ctx[0];
    od_sel[0] = sub;
    od_sel[1] = 0x87;
    od_sel[2] = 0;
    od_sel[3] = 0;

    publish_ok = -1;
    entry = (uint32_t **)od_registry_lookup(ctx_id, *(uint32_t *)od_sel);
    if (entry != 0 && od_registry_wait(entry[1], 100) == 0) {
        uint8_t *dst = (uint8_t *)entry[0];
        const uint8_t *src = &scratch[1];
        int i;
        for (i = 0; i < 13; i++)
            dst[i] = src[i];
        publish_ok = 0;
        od_sel[0] = sub;
        od_sel[1] = 0x87;
        od_sel[2] = 0;
        entry = (uint32_t **)od_registry_lookup(ctx_id, *(uint32_t *)od_sel);
        if (entry != 0) {
            od_registry_release(entry[1]);
        } else {
            publish_ok = -1;
        }
    }

    if (rd_err != 0)
        publish_ok = -1;

    if (proxy == 0) {
        proxy = -1;
    } else {
        nvm_record_close(proxy);
        proxy = 0;
    }
    if (publish_ok != 0)
        proxy = -1;

    if (out != 0) {
        int cmp = mem_compare(scratch, (const void *)(uintptr_t)out, 0xe);
        if (proxy == 0)
            proxy = -(int)(cmp != 0);
        else
            proxy = -1;
    }
    return proxy;
}

/*
 * elock_load_aeskey_to_od_91 — read the 128-bit AES key from NVM and publish
 * the full 16 bytes into the comms Object-Dictionary descriptor 0x91xx.
 *
 * OEM disassembly (0x0000695a..0x00006a42):
 *
 * Identical shape to elock_load_secret_to_od_87 but for the 16-byte AES key
 * record. Opens the proxy, zeroes a 16-byte scratch, and — if the proxy opened —
 * device-reads record 0x40 of length 16; the read result is normalised to 0/-1.
 * There is no tag-byte sentinel check (the key has no length prefix). The OD
 * selector is {sub, 0x91, 0}; all 16 key bytes are copied into the entry data
 * buffer; the entry is re-looked-up and released. rd_err, publish_ok and the
 * proxy-close status collapse to a 0/-1 return; an optional out pointer is
 * verified and merged.
 */
int elock_load_aeskey_to_od_91(uint32_t *ctx, int out)
{
    int proxy;
    int rd_err;
    int publish_ok;
    uint8_t od_sel[4];
    uint8_t scratch[16];
    uint32_t **entry;
    uint32_t ctx_id;
    uint8_t sub;

    proxy = nvm_record_open();
    mem_set(scratch, 0, 0x10);
    if (proxy == 0) {
        rd_err = -1;
    } else {
        rd_err = nvm_record_read(proxy, scratch, 0x40, 0x10);
        rd_err = -(int)(rd_err != 0);
    }

    sub    = *(uint8_t *)((uint8_t *)ctx + 4);
    ctx_id = ctx[0];
    od_sel[0] = sub;
    od_sel[1] = 0x91;
    od_sel[2] = 0;
    od_sel[3] = 0;

    publish_ok = -1;
    entry = (uint32_t **)od_registry_lookup(ctx_id, *(uint32_t *)od_sel);
    if (entry != 0 && od_registry_wait(entry[1], 100) == 0) {
        uint8_t *dst = (uint8_t *)entry[0];
        int i;
        for (i = 0; i < 16; i++)
            dst[i] = scratch[i];
        publish_ok = 0;
        od_sel[0] = sub;
        od_sel[1] = 0x91;
        od_sel[2] = 0;
        entry = (uint32_t **)od_registry_lookup(ctx_id, *(uint32_t *)od_sel);
        if (entry != 0) {
            od_registry_release(entry[1]);
        } else {
            publish_ok = -1;
        }
    }

    if (rd_err != 0)
        publish_ok = -1;

    if (proxy == 0) {
        proxy = -1;
    } else {
        nvm_record_close(proxy);
        proxy = 0;
    }
    if (publish_ok != 0)
        proxy = -1;

    if (out != 0) {
        int cmp = mem_compare(scratch, (const void *)(uintptr_t)out, 0x10);
        if (proxy == 0)
            proxy = -(int)(cmp != 0);
        else
            proxy = -1;
    }
    return proxy;
}

/*
 * elock_store_secret_87 — persist the lock secret to NVM and re-publish it.
 *
 * OEM disassembly (0x00006ac2..0x00006b20):
 *
 * The caller supplies the OD/ctx object and a pointer to the 13-byte secret
 * body. A 14-byte staging record is built on the stack: byte 0 is the
 * tag/length sentinel = 1, followed by the 13 secret bytes. The proxy is opened;
 * if it opened the 14-byte record is written-and-verified to record 0x30
 * (nvm_record_write_verify), the result kept as wr_err, then committed
 * (nvm_record_finalize, flag 1) normalised to 0/-1, and the proxy closed.
 * The freshly written secret is then ALWAYS re-read and re-published into OD
 * 0x87xx by elock_load_secret_to_od_87 (the reload runs even on write failure);
 * only afterward is the write status applied — returns -1 on open/write/finalize
 * failure, else the reload result.
 */
int elock_store_secret_87(uint32_t *ctx, int unused, const void *secret)
{
    int proxy;
    int wr_err;
    int rc;
    uint8_t rec[14];

    (void)unused;

    proxy = nvm_record_open();
    if (proxy == 0)
        return -1;

    rec[0] = 1;                             /* tag / length sentinel */
    mem_copy(&rec[1], secret, 0xd);         /* 13-byte body */

    wr_err = nvm_record_write_verify(proxy, rec, 0xe, 0);
    {
        int fin = nvm_record_finalize(proxy, 1);
        if (wr_err == 0)
            wr_err = -(int)(fin != 0);
        else
            wr_err = -1;
    }
    nvm_record_close(proxy);

    /* reload/republish unconditionally; the write status overrides the result */
    rc = elock_load_secret_to_od_87(ctx, (int)(uintptr_t)rec);
    if (wr_err != 0)
        return -1;
    return rc;
}

/*
 * elock_store_aeskey_91 — persist the 128-bit AES key to NVM and re-publish it.
 *
 * OEM disassembly (0x00006b22..0x00006b78):
 *
 * Mirrors elock_store_secret_87 for the 16-byte key (no tag/length prefix). The
 * 16 key bytes are staged, the proxy opened, the record written-and-verified to
 * record 0x40, committed (finalize flag 1) and the proxy closed; the key is then
 * ALWAYS reloaded and re-published into OD 0x91xx via elock_load_aeskey_to_od_91
 * (the reload runs even on write failure), and only afterward is the write status
 * applied. Returns -1 on failure, else the reload result.
 *
 * Note: this function has NO callers in the image — the elock never rotates its
 * AES key or secret at runtime (see also elock_store_secret_87); the key/secret
 * are only ever LOADED from NVM at boot by elock_main_init.
 */
int elock_store_aeskey_91(uint32_t *ctx, int unused, const void *aeskey)
{
    int proxy;
    int wr_err;
    int rc;
    uint8_t rec[16];

    (void)unused;

    proxy = nvm_record_open();
    if (proxy == 0)
        return -1;

    mem_copy(rec, aeskey, 0x10);            /* 16-byte key */

    wr_err = nvm_record_write_verify(proxy, rec, 0x10, 0x10);
    {
        int fin = nvm_record_finalize(proxy, 1);
        if (wr_err == 0)
            wr_err = -(int)(fin != 0);
        else
            wr_err = -1;
    }
    nvm_record_close(proxy);

    /* reload/republish unconditionally; the write status overrides the result */
    rc = elock_load_aeskey_to_od_91(ctx, (int)(uintptr_t)rec);
    if (wr_err != 0)
        return -1;
    return rc;
}
