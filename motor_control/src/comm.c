/*
 * comm.c -- VanMoof S5 motor_control (SCI + CAN + SPI communication).
 *
 * REFERENCE C, hand-translated from the IDA tms32028 disassembly of the TI
 * TMS320F280049C (C2000/C28x) motor-drive firmware (build/ida/functions.asm).
 * There is no C28x GCC backend, so -- like the S3 motorware -- this is reference
 * documentation, NOT compiled. Register names follow the TI C2000Ware bitfield
 * style (CpuSysRegs/AdcaRegs/EALLOW/...); HWREG(a) is a raw word access. FOC math
 * uses the C28x FPU/TMU (modelled as float). OEM addresses are in each header.
 */
#include "motor_control.h"


/* ===== scheduled_sci_packet_send  (OEM 0x0823A7, sci) =====
 * Iterates a table of 8 scheduled-transmit entries in GS RAM starting at 0xC5EA (each 13 words apart). On overflow of the word_C0C0 counter (==250) calls a reset handler (sub_82DCF) and updates motor state (sub_82FB1). For each entry: checks state field (*+entry[1]); 0=skip, 5=process-now, other=call sub_830EF to check if time to fire. When an entry fires, decrements its countdown; when countdown re
 * Peripherals: GS RAM table @0xC5EA (8 entries x 13 words); staging buffer @0xC6CC; word_C0C0 @0xC0C0 (state/counter); sub_82DCF (overflow handler); sub_82FB1 (motor state update, ACC=40); sub_830EF (timer check); sub_827C8 (SLIP encode+send)
 * [confidence: medium] */
/* 0x0823A7 - scheduled SCI packet transmit task */
/* SchedEntry: [0]=type, [1]=countdown, [2]=d0, [3]=d1_lo, [4]=d1_hi */
typedef struct { uint16_t type; uint16_t cnt; uint16_t d0; uint16_t d1l; uint16_t d1h; /* ... 13 words total */ } SchedEntry;

uint16_t scheduled_sci_packet_send(void) {
    /* Check overflow counter */
    if (word_C0C0 == 250) {
        word_C0C0 = reset_state();         /* sub_82DCF returns new word_C0C0 */
        motor_state_update(40, 0, word_C0C0); /* sub_82FB1(ACC=40,XAR4=0,AR5=word_C0C0) */
    }

    SchedEntry *entry    = (SchedEntry *)0xC5EA;
    SciTxBuf   *txbuf    = (SciTxBuf   *)0xC6CC;
    uint16_t    word_off = 0;   /* XAR2: byte offset accumulator */
    uint16_t    n        = 7;   /* 8 entries (banz 0..7) */

    do {
        uint16_t state = entry->cnt;
        if (state == 0) goto next;
        if (state != 5) {
            uint16_t ok = check_timer(word_C0C0); /* sub_830EF(AL=word_C0C0) */
            if (!ok) goto next;
        }

        /* Decrement and check if fired */
        motor_state_update(40, 0, word_C0C0); /* sub_82FB1 */
        if (state != 0) --entry->cnt;
        if (entry->cnt != 0) {
            uint32_t pkt_len;
            txbuf->type = 2;
            if (entry->type == 1) {
                /* 7-byte payload */
                txbuf->len      = 7;
                txbuf->data[0]  = (uint8_t)(entry->d0);
                txbuf->data[1]  = (uint8_t)(entry->d1l);
                txbuf->data[2]  = (uint8_t)(entry->d1l >> 8);
                txbuf->data[3]  = (uint8_t)(entry->d1h);
                txbuf->data[4]  = (uint8_t)(entry->d1h >> 8);
                /* 2 more bytes from entry fields */
                uint16_t extra = entry->d1h;
                if ((int16_t)extra >= 0 /* XAR7 vs XAR6 compare */) {
                    /* accumulate offset and write bytes */
                    txbuf->data[5] = (uint8_t)(extra);
                    txbuf->data[6] = (uint8_t)(extra >> 8);
                }
                pkt_len = 7;
            } else {
                /* 6-byte payload */
                txbuf->len      = 6;
                txbuf->data[0]  = (uint8_t)(entry->d0);
                txbuf->data[1]  = (uint8_t)(entry->d1l);
                txbuf->data[2]  = (uint8_t)(entry->d1l >> 8);
                txbuf->data[3]  = (uint8_t)(entry->d1h);
                txbuf->data[4]  = (uint8_t)(entry->d1h >> 8);
                pkt_len = 5;
            }
            slip_encode_send(pkt_len, txbuf); /* sub_827C8(ACC=pkt_len, XAR4=txbuf) */
        } else {
            return 0;
        }
next:
        word_off += 13;
        entry    = (SchedEntry *)((uint16_t *)0xC5EA + word_off);
    } while (n-- != 0);  /* banz */

    return 1;
}


/* ===== sci_fifo_init  (OEM 0x0825A0, sci) =====
 * Configures two SCI (UART) channels for FIFO operation. XAR6 and XAR7 are the two SCI base pointers; input ACC is a pointer to a SCI configuration struct. Sets SCIFFTX bits [14:13] (TX FIFO enable / interrupt clear) and SCIFFRX bits [14:13]. Programs baud related constants (25,000,000 Hz clock, 115,200 target baud) onto stack for use by sub_82E81 (baud rate calculator). Writes SCICTL2 bits 0,1,5 (T
 * Peripherals: SCIA @0x7200 / SCIB @0x7210 (offsets: SCIFFTX=+10, SCIFFRX=+11, SCICTL2=+1); SCIFFTX bit 13=TXFFIENA, bit 14=TXFFINTCLR; baud=115200; LSPCLK=25,000,000 Hz; sub_82E81 (baud calc), sub_82FE7 (enable), sub_82FCD (release reset)
 * [confidence: medium] */
/* 0x0825A0 - configure SCI-A and SCI-B FIFO mode, baud 115200 */
/* ACC = ptr to SCI config struct; XAR6/XAR7 = saved SCI handle ptrs (output) */
void sci_fifo_init(SciConfig *cfg, uint16_t **out_a, uint16_t **out_b) {
    volatile uint16_t *sci = *(volatile uint16_t **)cfg;  /* dereference config ptr */
    *out_a = (uint16_t *)sci;  /* save for caller (XAR6) */
    *out_b = (uint16_t *)sci;  /* save for caller (XAR7) */

    uint32_t lspclk  = 25000000UL;  /* 0x017D7840 */
    uint32_t baud    = 115200UL;
    uint8_t  fifo_tx = 7;   /* XAR7 immediate */

    volatile uint16_t *scifftx_a = sci + 10;
    volatile uint16_t *sciffrx_a = sci + 11;

    /* Disable TX/RX FIFO interrupts initially */
    *scifftx_a &= 0xDFFFU;  /* clear bit 13 (one SCI) */
    *sciffrx_a |= 0x2000U;  /* set bit 13 (other SCI) */
    /* mirror for second channel (XAR5/XAR4 pointers) */
    *(sci + 11) &= 0xDFFFU;
    *(sci + 11) |= 0x2000U;

    /* Compute baud and configure SCI via helper */
    sci_set_baud(lspclk, sci);   /* sub_82E81(ACC=16, ptr=sci) -- TODO verify args */

    /* Enable RX FIFO interrupt on channel 0: SCIFFRX bit */
    sci[1] |= 0x0023U;  /* SCICTL2: TXINTENA|RXBKINTENA|TXEMPTY */

    sci_enable(baud, sci);   /* sub_82FE7(ACC=1, ptr=sci) */
    sci_release_reset(sci);  /* sub_82FCD(ACC=1, ptr=sci) */

    /* Set SCIFFCT and priority bits */
    volatile uint16_t *sciffct = sci + 10;
    *(sci + 10) = (*(sci + 10) & 0xFFE0U) | 2U;   /* TXFFIL = 2 */
    *(sci + 11) = (*(sci + 11) & 0xFFE0U) | 1U;   /* RXFFIL = 1 */
    sci[1]     &= ~0x0020U;   /* clear bit 5 */
    sci[1]     |=  0x0020U;   /* set bit 5 (TXEMPTY) */

    /* Restore FIFO mode on both TX and RX of both SCI channels */
    *(sci + 10) &= 0xDFFFU;
    *(sci + 10) |= 0x2000U;
    *(sci + 11) &= 0xDFFFU;
    *(sci + 11) |= 0x2000U;
}


/* ===== slip_rx_byte  (OEM 0x08266A, sci) =====
 * SLIP (Serial Line IP) frame decoder — processes one incoming byte at a time. Uses a state variable at word_C0C0+1 (states: 0=idle/look-for-END, 1=in-frame, 2=after-ESC). On state 0: waits for 0xC0 (SLIP_END) to open a frame. On state 1: normal bytes appended to ring-buffer via write-to-tail; 0xC0 = frame complete (reset tail to 0); 0xDB = enter ESC state (2). On state 2: 0xDC -> write 0xC0; 0xDD -
 * Peripherals: word_C0C0+1 @0xC0C1 (SLIP state); ring buffer at XAR2 (base *XAR2, limit *+XAR2[2], tail *+XAR2[4]); SLIP constants: END=0xC0(192), ESC=0xDB(219), ESC_END=0xDC(220), ESC_ESC=0xDD(221); sub_83100 (buf get), sub_830B7 (flush)
 * [confidence: medium] */
/* 0x08266A - SLIP receive byte state machine */
/* XAR4 = RxRingBuf*; byte in *-SP[var_1] (pushed by caller) */
uint16_t slip_rx_byte(SlipRxBuf *buf, uint16_t byte) {
    uint16_t result  = 1;          /* AR1: default = 'frame in progress' */

    /* Outer loop: retry reading from hw buffer until no more bytes */
    while (1) {
        uint8_t state = word_C0C1; /* word_C0C0+1 */
        switch (state) {

        case 0: /* idle: look for frame-open END marker */
            if (byte == 0xC0U) {
                buf->tail = 0;      /* reset tail pointer */
                word_C0C1 = 1;      /* enter 'in frame' */
            }
            /* ignore all other bytes while idle */
            break;

        case 1: /* in frame */
            if (byte == 0xC0U) {
                /* END byte: frame complete */
                word_C0C1 = 0;
                return 0;  /* signal 'frame done' */
            } else if (byte == 0xDB) {
                word_C0C1 = 2;  /* ESC: wait for ESC_x */
            } else {
                /* normal byte: write to ring buffer */
                uint32_t tail = buf->tail;
                if (tail < buf->limit) {
                    buf->data[tail] = (uint8_t)byte;
                    buf->tail = tail + 1;
                }
            }
            break;

        case 2: /* after ESC */
            if (byte == 0xDDU) {
                byte = 0xDB;  /* ESC_ESC -> actual 0xDB */
                word_C0C1 = 1;
                /* fall through to write byte */ /* TODO verify */
                if (buf->tail < buf->limit)
                    buf->data[buf->tail++] = (uint8_t)byte;
            } else if (byte == 0xDCU) {
                byte = 0xC0;  /* ESC_END -> actual 0xC0 */
                word_C0C1 = 1;
                if (buf->tail < buf->limit)
                    buf->data[buf->tail++] = (uint8_t)byte;
            } else {
                result = 2;  /* unexpected byte after ESC */
                word_C0C1 = 0;
            }
            break;
        }

        /* Try to read next byte from hardware FIFO */
        byte = buf_get_byte(buf);  /* sub_830B7 */
        if (!buf_has_data(buf))    /* sub_830B7 returns 0 when empty */
            break;
    }
    return result;
}


/* ===== slip_encode_send  (OEM 0x0827C8, sci) =====
 * SLIP frame encoder and transmitter. Sends a byte sequence from input buffer (XAR1=src ptr, XAR1-length via ACC) byte-by-byte, escaping 0xC0 as DB DC and 0xDB as DB DD. After the payload, encodes a 16-bit closing word (AR3) with the same escape rules. Then writes the final SLIP_END byte 0xC0. Each byte is output via sub_82EC2 (SCI TX byte). Reads from ring buffer via sub_83100 (ring-buf byte fetch)
 * Peripherals: src ring buffer (XAR2=base, XAR5=limit 0xFFFF); sub_83100 (ring-buf read); sub_82EC2 (SCI byte send); SLIP constants: END=0xC0, ESC=0xDB, ESC_END=0xDC, ESC_ESC=0xDD
 * [confidence: high] */
/* 0x0827C8 - SLIP encode and transmit a frame */
/* ACC = src byte count; XAR4 = output ring-buf; XAR2 = src ring-buf */
void slip_encode_send(uint32_t len, TxRingBuf *out, RxRingBuf *src) {
    /* Fetch the src byte stream from ring buffer */
    uint32_t remaining = ringbuf_fetch(src, 0xFFFFUL); /* sub_83100 */
    uint16_t end_word  = (uint16_t)remaining;           /* AR3 = low byte of result */

    /* Emit leading SLIP END marker */
    tx_byte(0xC0U, out);  /* sub_82EC2 */

    /* Encode and transmit payload bytes */
    uint8_t *data = (uint8_t *)src;
    uint32_t n    = len;
    while (n > 0) {
        uint8_t b = *data++;
        if (b == 0xC0U) {
            tx_byte(0xDBU, out);  /* ESC */
            tx_byte(0xDCU, out);  /* ESC_END */
        } else if (b == 0xDBU) {
            tx_byte(0xDBU, out);  /* ESC */
            tx_byte(0xDDU, out);  /* ESC_ESC */
        } else {
            tx_byte(b, out);
        }
        n--;
    }

    /* Encode closing word low byte (AR3 & 0xFF) */
    {
        uint8_t lo = (uint8_t)(end_word);
        if      (lo == 0xC0U) { tx_byte(0xDBU, out); tx_byte(0xDCU, out); }
        else if (lo == 0xDBU) { tx_byte(0xDBU, out); tx_byte(0xDDU, out); }
        else                  { tx_byte(lo,    out); }
    }
    /* Encode closing word high byte (AR3 >> 8) */
    {
        uint8_t hi = (uint8_t)(end_word >> 8);
        if      (hi == 0xC0U) { tx_byte(0xDBU, out); tx_byte(0xDCU, out); }
        else if (hi == 0xDBU) { tx_byte(0xDBU, out); tx_byte(0xDDU, out); }
        else                  { tx_byte(hi,    out); }
    }

    /* Trailing SLIP END */
    tx_byte(0xC0U, out);
}


/* ===== spia_init  (OEM 0x082859, spi) =====
 * Initialises SPI-A. Reads the SPI-A base pointer from the peripheral handle struct at word-offset 28 (= 0x6100, SPIA). Clears SPICCR.CLKPOLARITY or loopback bit (AND 0xFF7F = clear bit 7). Sets stack parameters: 32-bit clock config 0x017D7840 (25,000,000 = LSPCLK in Hz), timeout 400,000, FIFO level 16. Calls sub_82EE2 (set SPI baud and mode) with SPIA base in ACC. Applies EALLOW-protected SPIBRR di
 * Peripherals: PeriphHandles struct[28] = SPIA @0x6100; SPICCR (+0) bit7=loopback; SPIBRR (+3); SPIFFTX (+8 or +9); SPIFFRX (+9 or +10); LSPCLK=25,000,000; timeout=400,000; sub_82EE2 (baud/mode), sub_82F20 (enable); EALLOW
 * [confidence: medium] */
/* 0x082859 - initialise SPI-A (handle struct offset 28 = 0x6100) */
void spia_init(PeriphHandles *h) {
    volatile uint16_t *spi = (volatile uint16_t *)h->words[28]; /* SPIA = 0x6100 */

    /* Disable loopback / reset CLKPOLARITY (clear bit 7 of SPICCR) */
    spi[0] &= 0xFF7FU;  /* SPICCR &= ~0x80 */

    /* Config params on stack */
    uint32_t lspclk  = 25000000UL;  /* 0x017D7840 */
    uint32_t timeout = 400000UL;
    uint8_t  fifo_lv = 16U;

    /* Set baud rate and SPI mode via helper */
    spi_set_baud(lspclk, spi);   /* sub_82EE2(ACC = SPIA base) */

    /* EALLOW: apply SPIBRR divider; set TXFFIENA */
    EALLOW;
    spi[0] &= 0xFFEFU;   /* clear bit 4 (some SPICCR field) */
    /* SPIFFTX: set bit 4 (TXFFINT clear) and bit 5 (TX level) */
    spi[11] = (spi[11] & 0xFFDFU) | 0x10U; /* TODO verify offset */
    spi[12]  = (spi[12] & 0x0000U) | 0x0018U; /* write *XAR4,#24 */ 
    EDIS;

    /* Enable SPIA */
    spi_enable(lspclk, spi);  /* sub_82F20 */

    /* Set SPICCR.SPILBK (bit 7) -- enable? */
    EALLOW;
    spi[0] |= 0x80U;  /* or *XAR4,#80h */
    EDIS;
}


/* ===== spib_init  (OEM 0x08289D, spi) =====
 * Identical structure to spia_init (0x082859) but operates on SPI-B using handle struct offset 30 (= 0x6110). Clears SPICCR loopback/polarity bit, configures baud via sub_82EE2 (25,000,000 Hz clock, 400,000 timeout, FIFO level 16), sets SPIFFTX, and enables SPI-B via sub_82F20.
 * Peripherals: PeriphHandles struct[30] = SPIB @0x6110; same register fields as spia_init; sub_82EE2 (baud), sub_82F20 (enable); LSPCLK=25,000,000; timeout=400,000; fifo_level=16
 * [confidence: medium] */
/* 0x08289D - initialise SPI-B (handle struct offset 30 = 0x6110) */
void spib_init(PeriphHandles *h) {
    volatile uint16_t *spi = (volatile uint16_t *)h->words[30]; /* SPIB = 0x6110 */

    spi[0] &= 0xFF7FU;   /* SPICCR: clear bit 7 (loopback off) */

    uint32_t lspclk  = 25000000UL;
    uint32_t timeout = 400000UL;
    uint8_t  fifo_lv = 16U;

    spi_set_baud(lspclk, spi);  /* sub_82EE2 */

    EALLOW;
    spi[0]  &= 0xFFEFU;  /* clear bit 4 */
    spi[11]  = (spi[11] & 0xFFDFU) | 0x10U; /* TODO verify offset */
    spi[12]  = 24U;      /* movb *XAR4,#24 */
    EDIS;

    spi_enable(lspclk, spi);  /* sub_82F20 */

    EALLOW;
    spi[0] |= 0x80U;  /* re-enable */
    EDIS;
}


/* ===== sci_channel_table_init  (OEM 0x082A20, sci) =====
 * Initialises a SCI channel descriptor struct (XAR2 = *XAR5). Zeroes all control fields at offsets 8-15, then calls sci_fifo_tx_byte (sub_82A5D) eight times with baud-rate pre-scale values 0, 2048, 4096, 6144, 8192, 10240, 12288, 14336 and stores each returned token at XAR2[0..7]. One caller.
 * Peripherals: SCI (via sub_82A5D); GSxRAM SCI descriptor struct pointed to by *XAR5; baud divider constants: 0, 2048, 4096, 6144, 8192, 10240, 12288, 14336
 * [confidence: medium] */
/* 0x082A20 - sci_channel_table_init */
extern uint16_t sci_fifo_tx_byte(uint16_t baud_div); /* sub_82A5D */

void sci_channel_table_init(SciChanDesc *ch)
{
    /* zero control fields */
    ch->field[11] = 0;
    ch->field[ 9] = 0;
    ch->field[13] = 0;
    ch->field[15] = 0;
    ch->field[10] = 0;
    ch->field[12] = 0;
    ch->field[14] = 0;
    ch->field[ 8] = 0;

    /* enumerate eight baud slots; store SCI response tokens */
    ch->slot[0] = sci_fifo_tx_byte(0);
    ch->slot[1] = sci_fifo_tx_byte(2048);
    ch->slot[2] = sci_fifo_tx_byte(4096);
    ch->slot[3] = sci_fifo_tx_byte(6144);
    ch->slot[4] = sci_fifo_tx_byte(8192);
    ch->slot[5] = sci_fifo_tx_byte(10240);
    ch->slot[6] = sci_fifo_tx_byte(12288);
    ch->slot[7] = sci_fifo_tx_byte(14336);
}


/* ===== sci_fifo_tx_byte  (OEM 0x082A5D, sci) =====
 * Transmits one byte through a SCI channel with FIFO management. Sets the SCI FIFO reset flag (bit15) in AL before transmission. Busy-waits 8 cycles, then via the pointer in *XAR4 locates the SCI peripheral, resets SCIFFRX (clears then sets bit13=RXFFIENA), writes the byte to SCITXBUF (offset+8), waits TX ready (polls bit5 of SCICTL1), then monitors RX FIFO level (SCIFFRX[11:8]) for up to 0xFFFE ite
 * Peripherals: SCI peripheral (SCIA 0x7200 / SCIB 0x7210) via pointer *XAR4; SCIFFRX offset+11 (bit13=RXFFIENA); SCITXBUF offset+8/9; SCICTL1 offset+1 (bit5=TXRDY); error flag at XAR4[6]
 * [confidence: medium] */
/* 0x082A5D - sci_fifo_tx_byte */
uint16_t sci_fifo_tx_byte(uint16_t byte_val)
{
    volatile uint16_t *sci = (volatile uint16_t *)*g_sci_port_ptr; /* *XAR4 */
    uint16_t retry_count = 0;
    uint16_t rx_level_prev = 0;
    int      rx_ok = 0;

    byte_val |= 0x8000u;   /* set SCI FIFO select / reset bit */

    /* 8-cycle bus setup delay */
    for (volatile int i = 7; i >= 0; i--) { __asm(" nop"); }

    /* reset RX FIFO: clear RXFFIENA then set it */
    sci[11] &= 0xDFFFu;   /* SCIFFRX: clear bit13 (RXFFIENA) */
    sci[11] |= 0x2000u;   /* SCIFFRX: set  bit13 (RXFFIENA) */

    /* 32-cycle sync delay */
    for (volatile int i = 31; i >= 0; i--) { __asm(" nop"); }

    /* wait TX buffer ready (SCICTL2 bit5 = TXRDY) */
    while (!(sci[2] & 0x0020u)) { /* TODO verify offset */ }

    /* send byte */
    sci[8] = byte_val;   /* SCITXBUF (word offset 8 from SCI base) */

    /* poll RX FIFO level until stable or exhausted */
    if (rx_ok == 0) {
        uint16_t level = (sci[11] & 0x1F00u) >> 8u;  /* SCIFFRX RX FIFO level field */
        rx_level_prev = level;
        do {
            retry_count++;
            if (retry_count >= 0xFFFEu)
                g_sci_port_ptr[6] = 1;   /* set timeout/error flag */ /* TODO verify */
            level = (sci[11] & 0x1F00u) >> 8u;
        } while (level == rx_level_prev);
    }

    retry_count = (uint16_t)-1;   /* sentinel */
    return sci[7] & 0x07FFu;      /* SCIRXBUF low 11 bits */
}


/* ===== sci_chan_lookup_and_queue_tx  (OEM 0x082A97, sci) =====
 * Searches the 8-entry SCI channel table at GSxRAM 0xC5EA (13 words per entry) for a record matching a given channel ID in AL, with transfer count in AH (capped at 8). On match: copies 'count' data words from XAR5 to the channel slot via sub_82F5C, then bulk-reads 13 words from a flash frame template into the channel record via 'pread', advancing and wrapping a global round-robin TX index at 0xC0C2.
 * Peripherals: GSxRAM SCI channel table 0xC5EA (8 x 13-word records); global TX slot index 0xC0C2; flash frame templates read via C28x pread instruction
 * [confidence: medium] */
/* 0x082A97 - sci_chan_lookup_and_queue_tx */
extern void sci_data_copy(uint16_t count, uint16_t *src, uint16_t *dst); /* sub_82F5C */
extern volatile uint8_t g_sci_tx_slot;  /* 0xC0C2 */

uint16_t sci_chan_lookup_and_queue_tx(uint16_t chan_id, uint16_t count,
                                      uint16_t *data_src, uint32_t *msg_slot)
{
#define SCI_CHAN_TABLE  ((uint16_t *)0xC5EAu)
#define SCI_RECORD_SZ   13u
#define SCI_CHAN_COUNT   8u

    if (count > 8u)
        return 0xFDu;   /* error: too many bytes */

    uint16_t *rec = SCI_CHAN_TABLE;
    for (uint16_t i = 0; i < SCI_CHAN_COUNT; i++, rec += SCI_RECORD_SZ) {
        if (rec[1] != chan_id)
            continue;

        /* copy payload into slot */
        sci_data_copy((uint16_t)count, data_src, (uint16_t *)msg_slot);

        /* read frame template (13 words) from flash via program-read bus */
        /* pread *XAR2++,*XAR7 (repeated 13 times) */
        for (int k = 0; k < 13; k++)
            rec[k] = ((volatile uint16_t *)msg_slot)[k]; /* TODO verify pread target */

        /* advance round-robin TX slot, wrap at 256 */
        g_sci_tx_slot = (g_sci_tx_slot + 1u) & 0xFFu;
        return i;
    }
    return 0xFFu;   /* not found */
}


/* ===== sci_chan_flush_pending  (OEM 0x082AD1, sci) =====
 * Checks if the SCI channel struct pointed to by *XAR5 has a pending retransmit flag ([9] != 0). If so, retransmits slots [0..5] via sci_fifo_tx_byte and clears the flag [9]. Then checks extra-byte flag [15]; if set, transmits the high-bits word at [11]<<11 and records the result at [13], clearing [15]. One caller.
 * Peripherals: SCI channel descriptor in GSxRAM (via *XAR5); baud slot constants 0,2048,4096,6144,8192,10240,12288; retransmit flag field[9], extra-byte flag field[15]
 * [confidence: medium] */
/* 0x082AD1 - sci_chan_flush_pending */
extern uint16_t sci_fifo_tx_byte(uint16_t byte_val); /* sub_82A5D */

void sci_chan_flush_pending(SciChanDesc **pp_chan)
{
    SciChanDesc *ch = *pp_chan;

    if (ch->field[9] != 0) {
        ch->slot[0] = sci_fifo_tx_byte(0);
        ch->slot[1] = sci_fifo_tx_byte(2048);
        ch->slot[2] = sci_fifo_tx_byte(4096);
        ch->slot[3] = sci_fifo_tx_byte(6144);
        ch->slot[4] = sci_fifo_tx_byte(8192);
        ch->slot[5] = sci_fifo_tx_byte(10240);
        ch->slot[6] = sci_fifo_tx_byte(12288);
        ch->field[9] = 0;      /* clear retransmit pending flag */
    }

    if (ch->field[15] != 0) {
        uint16_t extra_arg = ch->field[11] << 11u;  /* pack into high bits */
        ch->slot[6] = sci_fifo_tx_byte((uint16_t)extra_arg); /* TODO verify shift */
        ch->field[15] = 0;
        ch->field[13] = ch->slot[6];
    }
}


/* ===== can_cmd_dispatcher  (OEM 0x082B08, can) =====
 * Processes one received CAN command from the message buffer at *0xC0C4. Calls sub_8266A to check if a CAN message is available; if not, returns immediately. Reads the command type byte from msg[1] and dispatches: type 5 = register read (calls sub_8308D); type 6 = OD access (calls loc_81EDE with 24-bit address from msg[3..4]); type 7 = multi-byte write + SCI forward (builds a reply frame at GSxRAM 0
 * Peripherals: GSxRAM CAN RX buffer ptr 0xC0C4; reply buffer 0xC328; CAN peripheral (via sub_8266A/sub_827C8); OD address from msg[3..6]
 * [confidence: medium] */
/* 0x082B08 - can_cmd_dispatcher */
extern uint16_t can_rx_check(uint16_t *buf_ptr);   /* sub_8266A; XAR4=0xC0C4 */
extern void     can_tx_send(void);                  /* sub_827C8 */
extern void     sci_reg_read(uint16_t chan);         /* sub_8308D */
extern void     od_write_handler(uint32_t addr);    /* loc_81EDE */
extern void     can_tx_handler(void);               /* loc_81771 */

typedef struct { uint16_t pad; uint16_t cmd; uint16_t chan; uint16_t dl; uint16_t dh; uint16_t al; uint16_t ah; uint16_t extra; } CanRxMsg;

void can_cmd_dispatcher(void)
{
    volatile CanRxMsg **pp_msg = (volatile CanRxMsg **)0xC0C4u;
    if (can_rx_check((uint16_t *)0xC0C4u) != 0)
        return;

    CanRxMsg *m = *pp_msg;
    uint16_t cmd = m->cmd;  /* msg[1] */

    if (cmd == 5u) {
        sci_reg_read(m->chan);
    } else if (cmd == 6u) {
        uint32_t addr = ((uint32_t)m->dh << 8u) | m->dl;
        od_write_handler(addr);

        /* build ACK in reply buffer at 0xC328 */
        volatile uint16_t *reply = (volatile uint16_t *)0xC328u;
        reply[0] = 2;
        reply[1] = 5;
        reply[2] = m->chan;
        can_tx_send();
    } else if (cmd == 7u) {
        uint32_t addr = ((uint32_t)m->dh << 8u) | m->dl;
        uint16_t chan = m->chan;
        uint16_t data_lo = m->al;
        can_tx_handler();

        volatile uint16_t *reply = (volatile uint16_t *)0xC328u;
        reply[0] = 2;
        reply[1] = 5;
        reply[2] = chan;
        can_tx_send();
    }
}


/* ===== sci_boot_reset_and_wait  (OEM 0x082B3D, sci) =====
 * Issues a hardware reset pulse on the GPIO pin encoded in XAR4[4]: clears the pin (GPACLEAR, offset+4 into GPIO data block), waits ~16382 CPU cycles, then sets the pin (GPASET, offset+2), waits another ~16382 cycles. Then polls sci_fifo_tx_byte with arg 0 up to 1000 times waiting for bit10 (boot acknowledgement) in the return value. Sets XAR4[7]=1 if 999 retries elapsed. Final ~4M-cycle delay befor
 * Peripherals: GPIO data regs 0x7F00 (GPASET offset+2, GPACLEAR offset+4); SCI via sub_82A5D; delay constants 16382, 4194302; timeout flag ctx[7]; retry limit 1000
 * [confidence: medium] */
/* 0x082B3D - sci_boot_reset_and_wait */
extern uint16_t sci_fifo_tx_byte(uint16_t byte_val); /* sub_82A5D */

void sci_boot_reset_and_wait(SciBootCtx *ctx)
{
    uint32_t pin_desc  = ctx->pin_desc;   /* ctx[4] (32-bit) */
    uint32_t pin_group = (pin_desc >> 3u) & ~3u;       /* align to port */
    uint16_t pin_bit_lo, pin_bit_hi;
    /* compute GPIO data-register address */
    volatile uint16_t *gpio_dat = (volatile uint16_t *)((pin_group * 2u) + 0x7F00u);
    uint16_t pin_nr  = (uint16_t)(pin_desc & 0x1Fu);
    uint32_t mask    = 1u << pin_nr;
    pin_bit_lo = (uint16_t)(mask & 0xFFFFu);
    pin_bit_hi = (uint16_t)(mask >> 16u);

    /* assert reset: GPACLEAR */
    gpio_dat[4] = pin_bit_lo;    /* GPACLEAR[lo] */
    gpio_dat[5] = pin_bit_hi;
    for (volatile uint32_t d = 16382u; d; d--) { __asm(" nop"); }

    /* de-assert reset: GPASET */
    gpio_dat[2] = pin_bit_lo;    /* GPASET[lo]   */
    gpio_dat[3] = pin_bit_hi;
    for (volatile uint32_t d = 16382u; d; d--) { __asm(" nop"); }

    /* poll for SCI boot-loader ACK (bit10 in response) */
    uint16_t attempt = 0;
    while (1) {
        uint16_t resp = sci_fifo_tx_byte(0);
        if (resp & (1u << 10u))
            break;
        attempt++;
        if (attempt >= 999u)
            ctx->timeout_flag = 1;   /* ctx[7] */
        if (attempt >= 1000u)
            break;
    }

    /* long post-reset delay ~4M cycles */
    for (volatile uint32_t d = 4194302u; d; d--) { __asm(" nop"); }
}


/* ===== sci_rx_ring_buf_consume  (OEM 0x082CB6, sci) =====
 * Consumes one byte from the 128-entry circular SCI RX ring buffer at GSxRAM 0xC4C8, storing it at offset [9] of the target struct XAR2. Advances read-index 0xC554 (wrapping at 128) and decrements byte-count 0xC555. When the buffer empties, calls sub_82FCD (frame complete handler) with XAR2. Also calls sub_82FCD if status field XAR2[10]&0x1F00>>8 != 4 and the global flag 0xC0D5 is set. One caller.
 * Peripherals: SCI RX ring buffer 0xC4C8[128]; read-index 0xC554; byte-count 0xC555; TX-busy flag 0xC0D5; frame handler sub_82FCD
 * [confidence: medium] */
/* 0x082CB6 - sci_rx_ring_buf_consume */
extern void sci_frame_complete(uint32_t ctx_ptr, uint16_t flags); /* sub_82FCD */
extern volatile uint8_t  g_sci_rx_rdidx;  /* 0xC554 */
extern volatile uint8_t  g_sci_rx_count;  /* 0xC555 */
extern volatile uint8_t  g_sci_tx_busy;   /* 0xC0D5 */

#define SCI_RX_BUF  ((volatile uint16_t *)0xC4C8u)
#define SCI_RX_BUF_SZ 128u

void sci_rx_ring_buf_consume(uint32_t ctx_ptr)
{
    volatile uint16_t *msg = (volatile uint16_t *)ctx_ptr;

    /* fetch byte from ring buffer */
    msg[9] = SCI_RX_BUF[g_sci_rx_rdidx];
    g_sci_rx_count--;
    g_sci_rx_rdidx++;
    if (g_sci_rx_rdidx >= SCI_RX_BUF_SZ)
        g_sci_rx_rdidx = 0u;

    /* if buffer now empty: process the completed frame */
    if (g_sci_rx_count == 0u) {
        sci_frame_complete(ctx_ptr, 8u); /* flags=8 on stack TODO verify */
    }

    /* check SCI channel status word */
    uint16_t sci_state = (msg[10] & 0x1F00u) >> 8u;
    if (sci_state != 4u && g_sci_tx_busy != 0u) {
        sci_frame_complete(ctx_ptr, 8u);
    }
}


/* ===== spi_configure_baudrate  (OEM 0x082E81, spi) =====
 * Configures SPI baud rate and enables the SPI module. Takes a pointer to the SPI register block (XAR4), clock divisor parameters from stack (P=dividend, XAR0=extra config bits, ACC=divisor). Clears the SPIFFTX FIFO enable bit at +10 (and *XAR5[10] &= 0xBFFF). Clears SPICTL bits [1:0] at +1. Performs 32-bit unsigned division (rpt#31/subcul) to compute SPIBRR = (dividend/divisor) - 1. Writes quotient
 * Peripherals: SPI register block at XAR4: +0=SPICCR (char length/reset), +1=SPICTL (master/enable/talk), +2=SPIHBAUD (baud high), +3=SPILBAUD (baud low), +10=SPIFFTX (FIFO control, bit 14=FFENA). Stack: arg_2=clock dividend (32-bit), arg_4=baud divisor, arg_6=SPICCR base flags.
 * [confidence: medium] */
/* @0x082E81 - SPI baud rate configuration and module enable
 * XAR4 = SPI register block pointer
 * XAR5 = same pointer (for FIFO control offset)
 * stack[arg_2] = 32-bit clock dividend (P register)
 * stack[arg_4] = 32-bit divisor (desired baud rate or prescaler input)
 * stack[arg_6] = base SPICCR flags (AR0)
 */
typedef struct {
    uint16_t SPICCR;   /* +0 */
    uint16_t SPICTL;   /* +1 */
    uint16_t SPIHBAUD; /* +2 */
    uint16_t SPILBAUD; /* +3 */
    uint16_t _pad[6];
    uint16_t SPIFFTX;  /* +10 */
} SPIRegs;

void spi_configure_baudrate(SPIRegs *spi, uint32_t dividend, uint32_t divisor, uint16_t ccr_flags)
{
    uint32_t brr;
    uint16_t brr_lo, brr_hi;

    /* Disable SPI FIFO (clear SPIFFTX bit 14) and clear SPICTL bits[1:0] */
    spi->SPIFFTX &= 0xBFFFu; /* clear FFENA */
    spi->SPICTL  &= 0xFFFCu; /* clear MASTER/TALK enables */

    /* Compute SPIBRR = (dividend / divisor) - 1 using 32/32 unsigned division */
    /* Implemented as rpt#31 / subcul (C28x unsigned divide instruction) */
    brr     = (uint32_t)(dividend / divisor) - 1u; /* TODO verify rounding */
    brr_hi  = (uint16_t)((brr >> 8) & 0xFFu);
    brr_lo  = (uint16_t)(brr & 0xFFu);

    spi->SPIHBAUD = brr_hi;
    spi->SPILBAUD = brr_lo;

    /* Merge supplied char-length flags into SPICCR (keep bits [15:8] and [4:3]) */
    spi->SPICCR = (spi->SPICCR & 0xFF18u) | ccr_flags;

    /* Enable SPI: set SPICTL bits [5:0]=0x23 (MASTER|TALK|CLKPHASE|ena) */
    spi->SPICTL |= 0x0023u;
}


/* ===== sci_tx_enqueue_byte  (OEM 0x082EC2, sci) =====
 * Enqueues one byte (AL) into the SCI TX software ring buffer and triggers transmission. Checks if byte count at 0xC0D5 >= 128 (buffer full: returns 0). Otherwise writes AL to ring buffer at 0xC4C8[write_index], increments count (0xC0D5) and write index (0xC0D3), wraps index at 128. Then reads *+XAR5[26] (SCI peripheral status word) and calls sub_82FE7 to flush. Returns 1 on success. 11 callers.
 * Peripherals: GSxRAM TX ring buffer at 0xC4C8 (128 words); byte count at 0xC0D5; write index at 0xC0D3; SCI struct pointer at 0xC34C (XAR5 = word_C34C); SCI register +26 word offset (SCIFFTX status). Calls sub_82FE7 (TX flush trigger).
 * [confidence: high] */
/* @0x082EC2 - Enqueue one byte to SCI TX ring buffer and trigger transmit
 * AL = byte to transmit
 * Returns: AL=1 on success, AL=0 if buffer full
 */
extern void sci_tx_flush(uint32_t count_and_reg); /* sub_82FE7 */

extern uint16_t sci_tx_count;      /* 0xC0D5 */
extern uint16_t sci_tx_wr_idx;     /* 0xC0D3 */
extern uint16_t sci_tx_buf[128];   /* 0xC4C8 */
extern void    *sci_struct_ptr;    /* 0xC34C - points to SCI peripheral struct */

uint16_t sci_tx_enqueue_byte(uint16_t byte_val)
{
    uint16_t *sci_regs;
    uint32_t flush_arg;

    if (sci_tx_count >= 128u)
        return 0u; /* buffer full */

    /* Store byte at write index */
    sci_tx_buf[sci_tx_wr_idx] = byte_val & 0xFFu;
    sci_tx_count++;
    sci_tx_wr_idx++;
    if (sci_tx_wr_idx >= 128u)
        sci_tx_wr_idx = 0u;

    /* Trigger transmit: read SCI status word at offset +26, pass count=8 */
    sci_regs = (uint16_t *)sci_struct_ptr;
    flush_arg = ((uint32_t)8u << 16) | (uint32_t)(*(uint32_t *)&sci_regs[26]);
    sci_tx_flush(flush_arg); /* sub_82FE7 - TODO verify arg packing */

    return 1u;
}


/* ===== sci_configure_uart  (OEM 0x082EE2, sci) =====
 * Configures SCI/UART character format and baud rate. Takes SCI register block pointer (XAR7), parity (AR4[0]), address-idle mode (AR4[1]), character length (AR5), and clock/baud parameters from stack (XT=dividend 32-bit, P=divisor 32-bit). Sets SCICCR bits: parity (bit 6), address-idle (bit 3), char length [2:0]. Writes SCICTL1 flags from AR6. Computes baud divider with rpt#31/subcul (P/XT unsigned
 * Peripherals: SCI register block at XAR7: +0=SCICCR (char length[2:0], parity[6], addr-idle[3]); +1=SCICTL1 (control flags, mask 0xFFF1 = clear bits[3:1]); +4=SCILBAUD (baud rate divisor). Stack args: arg_2=P(divisor), arg_4=XT(dividend), arg_5=AR6(SCICTL flags).
 * [confidence: high] */
/* @0x082EE2 - Configure SCI/UART character format and baud rate
 * XAR7   = SCI register block pointer
 * AR4    = flags: bit[0]=parity, bit[1]=address-idle-mode
 * AR5    = character length field [2:0] (e.g. 7 = 8-bit chars)
 * stack[arg_5] = SCICTL1 mode bits (e.g. RXENA|TXENA)
 * stack[arg_4] = 32-bit dividend (clock frequency)
 * stack[arg_2] = 32-bit divisor (baud rate)
 */
typedef struct {
    uint16_t SCICCR;   /* +0 */
    uint16_t SCICTL1;  /* +1 */
    uint16_t SCIHBAUD; /* +2 */
    uint16_t SCILBAUD; /* +3 */
    uint16_t SCILBAUD2;/* +4 - actual baud reg on some variants */
} SCIRegs;

void sci_configure_uart(SCIRegs *sci, uint16_t parity, uint16_t addr_idle,
                        uint16_t char_len, uint16_t ctl1_flags,
                        uint32_t clk_freq, uint32_t baud_rate)
{
    uint16_t ccr;
    uint16_t baud_div;

    /* Build SCICCR: clear existing parity/addr-idle/charlen, set new values */
    ccr  = sci->SCICCR & 0xFFB0u;          /* mask: keep bits [7:5] only */
    ccr |= (uint16_t)((parity & 1u) << 6); /* bit 6 = PARITY */
    ccr |= (uint16_t)((addr_idle & 1u) << 3); /* bit 3 = ADDR/IDLE mode */
    /* AR6-1 folded into the write - see original XAR6 decrement at 82DEA */
    ccr |= (uint16_t)(char_len & 0x7u);    /* bits [2:0] = SCICHAR */
    sci->SCICCR = ccr;

    /* Set SCICTL1: keep bits [3,0] from mask 0xFFF1, OR in ctl1_flags */
    sci->SCICTL1 = (sci->SCICTL1 & 0xFFF1u) | (ctl1_flags & ~0xFFF1u);

    /* Compute baud rate divisor: BRR = (clk_freq / baud_rate) - 1 */
    /* Implemented in hardware as: rpt #31 || subcul ACC,@XT (unsigned divide) */
    baud_div = (uint16_t)(clk_freq / baud_rate) - 1u; /* TODO verify */
    sci->SCILBAUD2 = baud_div; /* +4 = SCILBAUD */
}


/* ===== sci_rx_fifo_drain  (OEM 0x082F01, sci) =====
 * Drains the SCI RX hardware FIFO into a software ring buffer at 0xC448. Reads SCIRXBUF (*+XAR4[7]) one byte at a time. If ring buffer count (0xC0D2) >= 128, sets overflow flag at 0xC574 and discards. Otherwise stores byte at ring buffer[write_index] (0xC0D0), increments count and write index (wrapping at 128). Repeats while SCIFFRX.RXFFST (*+XAR4[11] & 0x1F00 >> 8) != 0. 1 caller.
 * Peripherals: SCI registers at XAR4: +7=SCIRXBUF (received byte, low byte used); +11=SCIFFRX (RX FIFO status, bits[12:8]=RXFFST count). GSxRAM: 0xC448=RX ring buffer (128 words); 0xC0D2=byte count; 0xC0D0=write index; 0xC574=overflow flag.
 * [confidence: high] */
/* @0x082F01 - Drain SCI RX hardware FIFO into software ring buffer
 * XAR4 = pointer to SCI register block struct
 */
extern uint16_t sci_rx_count;      /* 0xC0D2 */
extern uint16_t sci_rx_wr_idx;     /* 0xC0D0 */
extern uint16_t sci_rx_buf[128];   /* 0xC448 */
extern uint16_t sci_rx_overflow;   /* 0xC574 */

typedef struct {
    uint16_t pad[7];
    uint16_t SCIRXBUF;  /* +7 */
    uint16_t pad2[3];
    uint16_t SCIFFRX;   /* +11: bits[12:8]=RXFFST */
} SCIRxRegs;

void sci_rx_fifo_drain(SCIRxRegs *sci)
{
    uint16_t rx_byte;
    uint16_t fifo_cnt;

    do {
        /* Check SCIFFRX.RXFFST: bits [12:8] = number of words in RX FIFO */
        fifo_cnt = (sci->SCIFFRX & 0x1F00u) >> 8u;
        if (fifo_cnt == 0u)
            break;

        rx_byte = sci->SCIRXBUF & 0x00FFu; /* read received byte */

        if (sci_rx_count >= 128u)
        {
            /* Ring buffer full - set overflow flag and discard */
            sci_rx_overflow = 1u;
        }
        else
        {
            /* Store in ring buffer */
            sci_rx_buf[sci_rx_wr_idx] = rx_byte;
            sci_rx_count++;
            sci_rx_wr_idx++;
            if (sci_rx_wr_idx >= 128u)
                sci_rx_wr_idx = 0u;
        }
    } while (1);
}


/* ===== sci_apply_feature_flags  (OEM 0x082F20, sci) =====
 * Applies a set of SCI/peripheral feature flags from a bitmask (stack arg) to a peripheral register block (XAR4 pointer). Bit 1: modify *XAR4[0] (SCICCR) bit 7 (stop bits). Bit 0: set *+XAR4[2] bit 7 (RX mode or break detect). Bit 3: set bit 6 in *(*XAR4+10) (SCIFFTX TXFFIL or similar threshold). Bit 2: set bit 6 in *(*XAR4+11) (SCIFFRX threshold). Bit 4: set bit 14 in *+XAR4[11] (overflow clear or 
 * Peripherals: Peripheral register block at XAR4: +0=SCICCR (bit 7=STOPBITS), +2=control reg (bit 7=RX mode), +10/*ptr=SCIFFTX-like (bit 6=TX FIFO threshold bit), +11/*ptr=SCIFFRX-like (bit 6=RX FIFO threshold bit), +11=control reg (bit 14=FIFO overflow/enable). Bitmask from stack arg.
 * [confidence: medium] */
/* @0x082F20 - Apply SCI feature flags from bitmask to peripheral registers
 * XAR4  = peripheral struct pointer
 * stack[arg_2] = feature bitfield:
 *   bit 1: STOPBITS (set SCICCR bit 7)
 *   bit 0: RX special mode (set reg+2 bit 7)
 *   bit 3: TX FIFO threshold bit 6 (via deref +10)
 *   bit 2: RX FIFO threshold bit 6 (via deref +11)
 *   bit 4: FIFO enable/overflow clear bit 14 at +11
 */
void sci_apply_feature_flags(volatile uint16_t *sci, uint16_t flags)
{
    volatile uint16_t *inner;

    /* Bit 1: set/clear SCICCR stop bit (bit 7) */
    if (flags & (1u << 1))
    {
        sci[0] &= 0xFF7Fu; /* clear bit 7 first */
        sci[0] |= 0x0080u; /* then set bit 7 (STOPBITS=2) */
    }

    /* Bit 0: set bit 7 of register +2 (RX parity/break enable) */
    if (flags & (1u << 0))
    {
        sci[2] |= 0x0080u;
    }

    /* Bit 3: set bit 6 of inner peripheral register at ptr+10 */
    if (flags & (1u << 3))
    {
        inner = (volatile uint16_t *)*(volatile uint32_t *)sci; /* *sci as ptr */
        inner += 10;
        *inner |= 0x0040u; /* set bit 6 (TX FIFO threshold) TODO verify */
    }

    /* Bit 2: set bit 6 of inner peripheral register at ptr+11 */
    if (flags & (1u << 2))
    {
        inner = (volatile uint16_t *)*(volatile uint32_t *)sci;
        inner += 11;
        *inner |= 0x0040u; /* set bit 6 (RX FIFO threshold) TODO verify */
    }

    /* Bit 4: set bit 14 of sci[11] (FIFO enable or overflow clear) */
    if (flags & (1u << 4))
    {
        sci[11] |= 0x4000u;
    }
}


/* ===== can_mailbox_data_copy  (OEM 0x082F5C, can) =====
 * Copies data words from a source buffer (XAR5) into a destination pointer stored in a CAN struct (*XAR4). AL words are copied in the first pass (word-by-word). Then if ACC (extended count from AH) is non-zero, copies up to AH * 0x3FFFFF additional words using a nested BANZ loop. Likely fills a CAN message data field or DMA scatter buffer. 3 callers.
 * Peripherals: No direct peripheral registers. GSxRAM: XAR4 = pointer to CAN descriptor struct where field [0] = destination word pointer. XAR5 = source data buffer. AL = word count (0-8 for standard CAN DLC). AH = extended count for CAN FD or multi-frame. Constant 0x3FFFFE used as large BANZ counter.
 * [confidence: low] */
/* @0x082F5C - Copy data words to CAN mailbox destination via struct pointer
 * AL   = primary word count to copy
 * AH   = extended copy count (AH>0 triggers secondary nested copy)
 * XAR4 = pointer to CAN descriptor struct; struct[0] = dest word ptr
 * XAR5 = source data buffer pointer
 * TODO verify: extended copy semantics (0x3FFFFE loop counter unclear)
 */
void can_mailbox_data_copy(uint32_t *can_desc, uint16_t *src, uint16_t word_count, uint16_t ext_count)
{
    uint16_t *dest = (uint16_t *)*can_desc; /* deref struct field[0] = dest ptr */
    uint16_t i;
    uint16_t j;

    /* Primary copy: copy word_count words from src to dest */
    for (i = 0; i < word_count; i++)
        dest[i] = src[i];

    /* Check extended count (AH) */
    if (word_count == (uint16_t)(*(uint32_t *)&word_count) && ext_count == 0u) /* TODO verify condition */
        goto done;

    /* Extended (secondary) copy when AH != 0 */
    if (ext_count != 0u)
    {
        uint16_t *p_src  = src + word_count;
        uint16_t *p_dest = dest + word_count;
        for (j = 0; j < ext_count; j++)
        {
            /* Inner: copy 0x3FFFFF words -- actual count from AR6=0x3FFFFE+1; TODO verify */
            uint32_t k;
            for (k = 0; k <= 0x3FFFFEu; k++)
                *p_dest++ = *p_src++;
        }
    }

done:
    *can_desc = (uint32_t)dest; /* update pointer in struct to reflect new position? TODO */
}


/* ===== sci_port_configure_rx  (OEM 0x082F79, sci) =====
 * Conditionally enables RXENA (SCICTL1 bit5) and TX/RX FIFO interrupt flags in a SCI port register block. Takes a 32-bit pointer to the SCI HW register base in ACC and a 32-bit feature-flag word on the stack. If any of bits {0,1,5,6,7} are set in flags, clears then sets SCICTL1[5] (RXENA). If bit3 is set, sets bit6 of SCIFFRX ([base+10]). If bit4 is set, sets bit6 of SCIFFCT ([base+11]).
 * Peripherals: SCICTL1 @ [base+1] bit5=RXENA; SCIFFRX @ [base+10] bit6; SCIFFCT @ [base+11] bit6. SCI-A=0x7200, SCI-B=0x7210.
 * [confidence: medium] */
/* 0x082F79 - sci_port_configure_rx */
void sci_port_configure_rx(volatile uint16_t *sci_base, uint32_t flags)
{
    uint16_t f = (uint16_t)flags;
    /* if any of bits 0,1,5,6,7 set -> enable RXENA */
    if (f & (BIT0 | BIT1 | BIT5 | BIT6 | BIT7)) {
        sci_base[1] &= ~0x0020u; /* clear RXENA */
        sci_base[1] |=  0x0020u; /* set   RXENA */
    }
    if (f & BIT3) {
        volatile uint16_t *fifo_rx = (volatile uint16_t *)sci_base + 10;
        *fifo_rx |= 0x0040u; /* set bit6 of SCIFFRX */
    }
    if (f & BIT4) {
        volatile uint16_t *fifo_ct = (volatile uint16_t *)sci_base + 11;
        *fifo_ct |= 0x0040u; /* set bit6 of SCIFFCT */
    }
}


/* ===== sci_port_disable_selected  (OEM 0x082FCD, sci) =====
 * Clears selected SCI enable bits in a register block pointed to by ACC, controlled by a 5-bit flags word on the stack. bit0 -> clear SCICTL1[6] (TXENA); bit1 -> clear SCICTL2[1] (RXBKINTENA); bit2 -> clear SCICTL2[0] (TXINTENA); bit3 -> clear SCIFFRX[5] (at [base+10]); bit4 -> clear SCIFFCT[5] (at [base+11]).
 * Peripherals: SCICTL1 @ [base+1] bit6=TXENA; SCICTL2 @ [base+4] bit1=RXBKINTENA bit0=TXINTENA; SCIFFRX @ [base+10] bit5; SCIFFCT @ [base+11] bit5. SCI-A=0x7200, SCI-B=0x7210.
 * [confidence: medium] */
/* 0x082FCD - sci_port_disable_selected */
void sci_port_disable_selected(volatile uint16_t *sci_base, uint32_t flags)
{
    uint16_t f = (uint16_t)flags;
    if (f & BIT0) sci_base[1]  &= ~0x0040u; /* clear TXENA  */
    if (f & BIT1) sci_base[4]  &= ~0x0002u; /* clear RXBKINTENA */
    if (f & BIT2) sci_base[4]  &= ~0x0001u; /* clear TXINTENA */
    if (f & BIT3) {
        volatile uint16_t *p = (volatile uint16_t *)sci_base + 10;
        *p &= ~0x0020u; /* clear bit5 of SCIFFRX */
    }
    if (f & BIT4) {
        volatile uint16_t *p = (volatile uint16_t *)sci_base + 11;
        *p &= ~0x0020u; /* clear bit5 of SCIFFCT */
    }
}


/* ===== sci_port_enable_selected  (OEM 0x082FE7, sci) =====
 * Sets selected SCI enable bits in a register block pointed to by ACC, controlled by a 5-bit flags word on the stack. Exact inverse of sub_82FCD: bit0 -> set SCICTL1[6] (TXENA); bit1 -> set SCICTL2[1] (RXBKINTENA); bit2 -> set SCICTL2[0] (TXINTENA); bit3 -> set SCIFFRX[5] (at [base+10]); bit4 -> set SCIFFCT[5] (at [base+11]).
 * Peripherals: SCICTL1 @ [base+1] bit6=TXENA; SCICTL2 @ [base+4] bit1=RXBKINTENA bit0=TXINTENA; SCIFFRX @ [base+10] bit5; SCIFFCT @ [base+11] bit5. SCI-A=0x7200, SCI-B=0x7210.
 * [confidence: medium] */
/* 0x082FE7 - sci_port_enable_selected */
void sci_port_enable_selected(volatile uint16_t *sci_base, uint32_t flags)
{
    uint16_t f = (uint16_t)flags;
    if (f & BIT0) sci_base[1]  |= 0x0040u; /* set TXENA  */
    if (f & BIT1) sci_base[4]  |= 0x0002u; /* set RXBKINTENA */
    if (f & BIT2) sci_base[4]  |= 0x0001u; /* set TXINTENA */
    if (f & BIT3) {
        volatile uint16_t *p = (volatile uint16_t *)sci_base + 10;
        *p |= 0x0020u; /* set bit5 of SCIFFRX */
    }
    if (f & BIT4) {
        volatile uint16_t *p = (volatile uint16_t *)sci_base + 11;
        *p |= 0x0020u; /* set bit5 of SCIFFCT */
    }
}


/* ===== sci_register_timer_channels  (OEM 0x083031, sci) =====
 * Allocates two software timer channels for SCI timing and registers them. Calls sub_82DCF twice to obtain free channel numbers, saves the channel IDs to GSxRAM (0xC33B, 0xC33C). First channel: period=1000, data=handler@0x82DF6 (TX timing callback). Second channel: period=200, data=handler@0x82F3E (RX timing callback). Both registered via sub_82FB1 (timer_channel_register).
 * Peripherals: GSxRAM: tx_timer_channel @ 0x0C33B; rx_timer_channel @ 0x0C33C. FLASH handler ptrs: 0x082DF6, 0x082F3E. Timer tables at 0x0C652/0x0C548.
 * [confidence: high] */
/* 0x083031 - sci_register_timer_channels */
extern uint16_t sub_82DCF(void);       /* allocate free timer channel */
extern uint16_t timer_channel_register(uint16_t ch, uint32_t period, void *data);

#define SCI_TX_TIMER_CH (*(volatile uint16_t *)0x0C33BuL)
#define SCI_RX_TIMER_CH (*(volatile uint16_t *)0x0C33CuL)

extern void sci_tx_timer_handler(void); /* handler @ 0x082DF6 */
extern void sci_rx_timer_handler(void); /* handler @ 0x082F3E */

void sci_register_timer_channels(void)
{
    uint16_t ch;

    ch = sub_82DCF();
    SCI_TX_TIMER_CH = ch;
    timer_channel_register(ch, 1000u, (void *)0x082DF6uL);

    ch = sub_82DCF();
    SCI_RX_TIMER_CH = ch;
    timer_channel_register(ch, 200u, (void *)0x082F3EuL);
}


/* ===== sci_tx_buf_pop  (OEM 0x0830B7, sci) =====
 * Pops one byte from the SCI TX circular buffer and writes it to the destination register pointed to by XAR4 (typically the SCITXBUF register). State: byte count at 0x0C0D2, read index (0-127) at 0x0C0D1. If count is zero, returns 0 immediately. Otherwise reads buf[0xC448][read_idx], writes it to *XAR4, decrements count, increments and wraps read index at 128, returns 1.
 * Peripherals: GSxRAM: TX circular buffer @ 0x0C448 (128 x 16-bit words); tx_count @ 0x0C0D2; tx_read_idx @ 0x0C0D1. Destination: *XAR4 = SCITXBUF (typically 0x7207 or 0x7217).
 * [confidence: high] */
/* 0x0830B7 - sci_tx_buf_pop */
#define SCI_TX_BUF    ((volatile uint16_t *)0x0C448uL)
#define SCI_TX_COUNT  (*(volatile uint16_t *)0x0C0D2uL)
#define SCI_TX_RDIDX  (*(volatile uint16_t *)0x0C0D1uL)

uint16_t sci_tx_buf_pop(volatile uint16_t *sci_txbuf /* XAR4 */)
{
    if (SCI_TX_COUNT == 0u)
        return 0u;
    uint16_t idx = SCI_TX_RDIDX;
    *sci_txbuf = SCI_TX_BUF[idx];
    SCI_TX_COUNT--;
    idx++;
    if (idx >= 128u)
        idx = 0u;
    SCI_TX_RDIDX = idx;
    return 1u;
}


/* ===== sci_tx_interrupt_enable  (OEM 0x0830CB, sci) =====
 * Sets up the SCI TX interrupt for a SCI driver object (passed in XAR4, saved to XAR1). Stores the TX ISR callback address 0x220103 into a callback slot pointed to by ACC (pre-call ACC = address of callback variable). Calls sub_82B71 to enable the interrupt in the CPU IER. Then computes reg_addr = 7 + obj->reg_base (32-bit add via obj[+4]) and sets bit5 of that register (SCICTL2.TXINTENA or similar)
 * Peripherals: SCI driver obj[+4] = 32-bit SCI reg base ptr; reg_base+7 bit5 (TXINTENA-related); CPU IER bit0 = INT1 enable. Callback slot set to 0x220103.
 * [confidence: medium] */
/* 0x0830CB - sci_tx_interrupt_enable */
extern void sub_82B71(uint32_t irq_spec); /* interrupt enable helper */

typedef struct {
    uint16_t      word0;
    uint16_t      word1;
    uint32_t      word2_3;
    uint32_t      reg_base; /* offset +4 (32-bit) */
    /* ... */
} SciDriverObj;

void sci_tx_interrupt_enable(void **callback_slot /* ACC */,
                              SciDriverObj *obj    /* XAR4 */)
{
    *callback_slot = (void *)0x220103uL; /* store TX ISR pointer */

    sub_82B71(/* irq args implicit */0);

    volatile uint16_t *ctrl_reg =
        (volatile uint16_t *)(uintptr_t)(7u + obj->reg_base); /* TODO verify +7 */
    EALLOW;
    *ctrl_reg |= 0x0020u; /* set bit5 (TXINTENA or similar) */
    EDIS;

    IER |= 1u; /* enable CPU INT1 */
}


/* ===== crc16_compute_buf  (OEM 0x083100, comm) =====
 * Computes CRC-16/ARC over a buffer of 'count' words. ACC=count, XAR4=buffer base pointer, XAR5 (AR5) = initial CRC (callers pass 0xFFFF). Pre-decrements XAR4 then increments at top of loop to walk the buffer; calls crc16_step_byte once per word. Returns AL = final 16-bit CRC.
 * Peripherals: GSx RAM buffer (address in XAR4); polynomial 0xA001 (inside callee)
 * [confidence: medium] */
/* @0x083100 - crc16_compute_buf */
/* ACC=count, XAR4=buf, AR5=init_crc; returns AL=crc */
uint16_t crc16_compute_buf(uint16_t count, uint16_t *buf, uint16_t init_crc) {
    uint32_t n = count;   /* saved into XAR7 */
    uint16_t crc = init_crc; /* AR5 */
    if (n == 0) return crc;
    buf--;                /* pre-decrement (083103) */
    do {
        buf++;            /* 083104: addb XAR4,#1 */
        uint16_t data = *buf;  /* 083106: mov AH,*XAR4 */
        crc = crc16_step_byte(crc, data); /* 083107: lcr sub_8312B */
        n--;
    } while (n != 0);
    return crc;
}


/* ===== can_dispatch_pending_queue  (OEM 0x08310F, can) =====
 * Drains the pending CAN/SCI message queue. Reads the 32-bit pending-item count from word_C010 (GSx 0xC010), zeroes it, then calls can_route_message (sub_82D32) that many times to dispatch each queued item to its registered channel callback.
 * Peripherals: GSx RAM word_C010 (0xC010) = pending item count; calls sub_82D32 (channel router)
 * [confidence: medium] */
/* @0x08310F - can_dispatch_pending_queue */
/* Drains the CAN message pending-item count and dispatches each. */
extern void can_route_message(void); /* sub_82D32 */
extern uint32_t g_pending_msg_count; /* word_C010 @0xC010 */

void can_dispatch_pending_queue(void) {
    uint32_t count = g_pending_msg_count; /* XAR1 = *word_C010 */
    g_pending_msg_count = 0;
    if (count == 0) return;
    do {
        can_route_message(); /* sub_82D32 */
        count--;
    } while (count != 0);
}


/* ===== crc16_step_byte  (OEM 0x08312B, comm) =====
 * Single-byte step of the CRC-16/ARC (LSB-first, polynomial 0xA001). XORs data byte (AH) into the low byte of the running CRC (AL), then processes all 8 bits: if LSB set, shift right and XOR with 0xA001; otherwise just shift right. Returns updated 16-bit CRC in AL.
 * Peripherals: None; pure computation. Polynomial constant: 0xA001
 * [confidence: high] */
/* @0x08312B - crc16_step_byte */
/* AL=crc_in, AH=data_byte; returns AL=crc_out */
uint16_t crc16_step_byte(uint16_t crc, uint16_t data_byte) {
    crc ^= data_byte; /* xor AL,@AH */
    for (int i = 0; i < 8; i++) { /* XAR6=7, banz loop = 8 iters */
        if (crc & 1) {
            crc >>= 1;
            crc ^= 0xA001u;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}


/* ===== can_channel_bitmap_clear  (OEM 0x08314F, can) =====
 * Clears the two active-channel bitmaps (byte_C33D and byte_C33E in GSx RAM) and zeroes 2 consecutive words at the callback-table pointer array base (0xC548). Sets AL=1 as return value. Called once during CAN channel initialization to reset all channel state.
 * Peripherals: GSx byte_C33D (0xC33D) = channel-active bitmap A; byte_C33E (0xC33E) = channel-active bitmap B; 0xC548 (50504 decimal) = callback table pointer array; DP page 0xC300
 * [confidence: medium] */
/* @0x08314F - can_channel_bitmap_clear */
extern uint16_t g_chan_active_a;    /* byte_C33D @0xC33D */
extern uint16_t g_chan_active_b;    /* byte_C33E @0xC33E */
extern uint32_t g_cb_table[2];     /* @0xC548 (50504 dec) */

uint16_t can_channel_bitmap_clear(void) {
    g_chan_active_a = 0;   /* mov @byte_C33D,#0 */
    g_chan_active_b = 0;   /* mov @byte_C33E,#0 */
    /* rpt #1 || mov *XAR4++,#0 : zero 2 words at 0xC548 */
    g_cb_table[0] = 0;
    g_cb_table[1] = 0;
    return 1; /* AL=1 (movb AL,#1 before store) */
}


/* ===== sci_port_init_enable_int  (OEM 0x08317B, sci) =====
 * Loads the SCI port configuration sub-object from XAR4[26] (offset 52 words into the parent driver struct), passes it to sub_825A0 (the full SCI peripheral initialiser including baud-rate, FIFO, and GPIO mux setup), then enables CPU interrupt group 9 (IER |= 0x100) which covers SCIA/SCIB receive/transmit and CAN-A interrupts on the F280049C PIE.
 * Peripherals: SCI peripheral (SCIA/SCIB via sub_825A0); IER register bit 8 = INT9 (CAN/SCI PIE group 9); parent struct offset 26 (word offset = SCI sub-object pointer)
 * [confidence: medium] */
/* @0x08317B - sci_port_init_enable_int */
/* XAR4 = ptr to driver object; XAR4[26] = SCI port config sub-object */
extern void sci_peripheral_init(void *sci_cfg); /* sub_825A0 */

void sci_port_init_enable_int(DriverObj_t *obj) {
    void *sci_cfg = *(void **)((uint16_t *)obj + 26); /* movb XAR0,#26; movl ACC,*+XAR4[AR0] */
    sci_peripheral_init(sci_cfg); /* lcr sub_825A0 */
    IER |= 0x100u; /* or IER,#100h : enable INT9 (SCI/CAN PIE group) */
}


/* ===== can_txqueue_init  (OEM 0x083193, can) =====
 * Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into the parent struct) and calls sub_82B3D (the CAN transmit-queue / mailbox initialiser). One caller at 0x808DD during the peripheral init sequence.
 * Peripherals: GSx RAM parent struct at XAR4; sub-object at word offset 62 (byte offset 124); delegates to sub_82B3D (CAN TX queue init)
 * [confidence: medium] */
/* @0x083193 - can_txqueue_init */
extern void can_txq_setup(void *drv); /* sub_82B3D */

void can_txqueue_init(DriverObj_t *obj) {
    void *drv = *(void **)((uint16_t *)obj + 62); /* movb XAR0,#62; movl XAR4,*+XAR4[AR0] */
    can_txq_setup(drv); /* lcr sub_82B3D */
}


/* ===== can_rxqueue_init  (OEM 0x083198, can) =====
 * Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into the parent struct) and calls sub_82AD1 (the CAN receive-queue / mailbox initialiser). Three callers at 0x8090C, 0x8099D, 0x80D58 during the peripheral init sequence.
 * Peripherals: GSx RAM parent struct at XAR4; sub-object at word offset 62; delegates to sub_82AD1 (CAN RX queue init)
 * [confidence: medium] */
/* @0x083198 - can_rxqueue_init */
extern void can_rxq_setup(void *drv); /* sub_82AD1 */

void can_rxqueue_init(DriverObj_t *obj) {
    void *drv = *(void **)((uint16_t *)obj + 62); /* movb XAR0,#62; movl XAR4,*+XAR4[AR0] */
    can_rxq_setup(drv); /* lcr sub_82AD1 */
}


/* ===== can_filter_init  (OEM 0x08319D, can) =====
 * Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into the parent struct) and calls sub_82A20 (the CAN message filter / acceptance-filter table initialiser). One caller at 0x808E5 during peripheral init.
 * Peripherals: GSx RAM parent struct at XAR4; sub-object at word offset 62; delegates to sub_82A20 (CAN filter init)
 * [confidence: medium] */
/* @0x08319D - can_filter_init */
extern void can_filter_setup(void *drv); /* sub_82A20 */

void can_filter_init(DriverObj_t *obj) {
    void *drv = *(void **)((uint16_t *)obj + 62); /* movb XAR0,#62; movl XAR4,*+XAR4[AR0] */
    can_filter_setup(drv); /* lcr sub_82A20 */
}


/* ===== can_mailbox_send  (OEM 0x0831A2, can) =====
 * Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into the parent struct) and calls sub_828E1 (the CAN mailbox transmit / send function). Three callers at 0x80902, 0x80997, 0x80D4D — all in the main CAN message dispatch loop.
 * Peripherals: GSx RAM parent struct at XAR4; sub-object at word offset 62; delegates to sub_828E1 (CAN TX send)
 * [confidence: medium] */
/* @0x0831A2 - can_mailbox_send */
extern void can_mb_transmit(void *drv); /* sub_828E1 */

void can_mailbox_send(DriverObj_t *obj) {
    void *drv = *(void **)((uint16_t *)obj + 62); /* movb XAR0,#62; movl XAR4,*+XAR4[AR0] */
    can_mb_transmit(drv); /* lcr sub_828E1 */
}
