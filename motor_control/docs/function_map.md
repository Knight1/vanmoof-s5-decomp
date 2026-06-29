# motor_control - function map (87 functions, C28x)

Hand-translated from `build/ida/functions.asm` (IDA tms32028). Reference C in `src/`.

| Addr | Name | Subsys | File | Conf | Role |
|---|---|---|---|---|---|
| `0x080000` | `codestart_entry` | startup | main.c | high | Entry point: single instruction that jumps to codestart_0 (C-runtime _c_int00). This is th |
| `0x081C8F` | `peripheral_clocks_init` | clock | clock.c | high | Enables and disables peripheral clocks via CpuSysRegs.PCLKCRx registers (0x5D322-0x5D34C). |
| `0x081D65` | `adc_soc_init` | adc | adc.c | medium | Initialises three ADC modules (A, B, C) whose base addresses are read from a struct at XAR |
| `0x081F94` | `dcc_clock_verify` | clock | clock.c | medium | Verifies the system oscillator frequency using DCC1 (Dual-Clock Comparator). Selects clock |
| `0x0820EC` | `sysclk_pll_init` | clock | clock.c | medium | Configures the system PLL and clock source. Checks ClkCfgRegs.X1CNT (0x5D22E) bit 0; if al |
| `0x0823A7` | `scheduled_sci_packet_send` | sci | comm.c | medium | Iterates a table of 8 scheduled-transmit entries in GS RAM starting at 0xC5EA (each 13 wor |
| `0x0824AE` | `periph_handle_table_init` | startup | main.c | medium | Fills a large 'peripheral handle' struct (minimum size 72 words checked at entry) with bas |
| `0x0825A0` | `sci_fifo_init` | sci | comm.c | medium | Configures two SCI (UART) channels for FIFO operation. XAR6 and XAR7 are the two SCI base  |
| `0x08266A` | `slip_rx_byte` | sci | comm.c | medium | SLIP (Serial Line IP) frame decoder — processes one incoming byte at a time. Uses a state  |
| `0x0827C8` | `slip_encode_send` | sci | comm.c | high | SLIP frame encoder and transmitter. Sends a byte sequence from input buffer (XAR1=src ptr, |
| `0x082814` | `adc_channel_select` | adc | adc.c | medium | Identifies which ADC module (A=1, B=2, C=0) from the input base address (0x7400/0x7480/0x7 |
| `0x082859` | `spia_init` | spi | comm.c | medium | Initialises SPI-A. Reads the SPI-A base pointer from the peripheral handle struct at word- |
| `0x08289D` | `spib_init` | spi | comm.c | medium | Identical structure to spia_init (0x082859) but operates on SPI-B using handle struct offs |
| `0x0828E1` | `adc_result_batch_process` | adc | adc.c | medium | Processes pending ADC conversion results from a result struct (XAR2/XAR5). Checks field [8 |
| `0x082923` | `pie_init` | isr | foc.c | high | Initialises the F28004x PIE (Peripheral Interrupt Expansion) controller. Disables global i |
| `0x082964` | `gpio_set_pin_attributes` | gpio | gpio.c | medium | EALLOW-protected GPIO attribute writer. Given a packed pin descriptor in ACC (bits [15:5]  |
| `0x082A20` | `sci_channel_table_init` | sci | comm.c | medium | Initialises a SCI channel descriptor struct (XAR2 = *XAR5). Zeroes all control fields at o |
| `0x082A5D` | `sci_fifo_tx_byte` | sci | comm.c | medium | Transmits one byte through a SCI channel with FIFO management. Sets the SCI FIFO reset fla |
| `0x082A97` | `sci_chan_lookup_and_queue_tx` | sci | comm.c | medium | Searches the 8-entry SCI channel table at GSxRAM 0xC5EA (13 words per entry) for a record  |
| `0x082AD1` | `sci_chan_flush_pending` | sci | comm.c | medium | Checks if the SCI channel struct pointed to by *XAR5 has a pending retransmit flag ([9] != |
| `0x082B08` | `can_cmd_dispatcher` | can | comm.c | medium | Processes one received CAN command from the message buffer at *0xC0C4. Calls sub_8266A to  |
| `0x082B3D` | `sci_boot_reset_and_wait` | sci | comm.c | medium | Issues a hardware reset pulse on the GPIO pin encoded in XAR4[4]: clears the pin (GPACLEAR |
| `0x082B71` | `interrupt_enable_save_intm` | isr | foc.c | medium | Enables one CPU or PIE interrupt while preserving and returning the previous INTM (global  |
| `0x082BA5` | `gpio_set_mux` | gpio | gpio.c | medium | EALLOW-protected GPIO multiplexer configuration. Given a pin descriptor in ACC (bits [15:8 |
| `0x082BD5` | `epwm_sync_aq_update` | pwm | pwm.c | medium | Checks whether a sync-event trigger has occurred (bits 7 and 5 of EPWM state word [5] at p |
| `0x082C32` | `epwm_clock_enable` | pwm | pwm.c | medium | EALLOW-protected EPWM/HRPWM clock-enable state machine. ACC on entry selects the state: 0  |
| `0x082C8B` | `event_queue_dispatch` | isr | foc.c | low | Walks a statically-allocated event queue stored in flash/data at 0x831A8..0x831C0. Compare |
| `0x082CB6` | `sci_rx_ring_buf_consume` | sci | comm.c | medium | Consumes one byte from the 128-entry circular SCI RX ring buffer at GSxRAM 0xC4C8, storing |
| `0x082CE1` | `spin_forever` | util | util.c | high | Infinite busy-wait loop (NOP then unconditional branch to itself). Used as a CPU halt / fa |
| `0x082CE3` | `fault_dispatcher` | isr | foc.c | medium | Error/fault chain dispatcher. Reads error flag byte at 0xC0CA; if all bits set (0xFFFF ==  |
| `0x082D0A` | `pll_switch_to_xtal_and_set_fmult` | clock | clock.c | medium | Switches the system oscillator source to the XTAL input (clears CLKSRCCTL1 bits [1:0]), wa |
| `0x082D32` | `sw_timer_tick_dispatch` | timer | pwm.c | high | Software timer tick: iterates channels 0-15. For each channel whose bit is set in the 16-b |
| `0x082D5A` | `epwm_force_output_and_write_compare` | pwm | pwm.c | medium | Forces EPWM action-qualifier outputs and writes a compare value. Performs a 7-cycle NOP sy |
| `0x082D81` | `gpio_ctrl_bitfield_write` | gpio | gpio.c | low | Writes a bit-field (2-bit value from AL[1:0]) into a GPIO control register at a computed a |
| `0x082DCF` | `sw_timer_channel_clear` | timer | pwm.c | medium | Searches channels 0-15 for a slot not yet marked in the armed bitmap at 0xC33D. When found |
| `0x082E1C` | `gpio_set_mux_4bit` | gpio | gpio.c | high | Sets a 4-bit GPIO mux field (2 bits from GPAMUX + 2 bits from GPAGMUX combined) for the pi |
| `0x082E3E` | `xtal_settle_and_x1cnt_check` | clock | clock.c | medium | Crystal oscillator settle sequence: calls ROM delay (unk_E65A) with count 2000, then loops |
| `0x082E60` | `gpio_output_write` | gpio | gpio.c | medium | Writes a single GPIO output pin high or low. Computes the GPIO data register word address  |
| `0x082E81` | `spi_configure_baudrate` | spi | comm.c | medium | Configures SPI baud rate and enables the SPI module. Takes a pointer to the SPI register b |
| `0x082EA2` | `gpio_direction_set` | gpio | gpio.c | high | Sets or clears a GPIO pin direction bit in the GPADIR register (or equivalent at offset +1 |
| `0x082EC2` | `sci_tx_enqueue_byte` | sci | comm.c | high | Enqueues one byte (AL) into the SCI TX software ring buffer and triggers transmission. Che |
| `0x082EE2` | `sci_configure_uart` | sci | comm.c | high | Configures SCI/UART character format and baud rate. Takes SCI register block pointer (XAR7 |
| `0x082F01` | `sci_rx_fifo_drain` | sci | comm.c | high | Drains the SCI RX hardware FIFO into a software ring buffer at 0xC448. Reads SCIRXBUF (*+X |
| `0x082F20` | `sci_apply_feature_flags` | sci | comm.c | medium | Applies a set of SCI/peripheral feature flags from a bitmask (stack arg) to a peripheral r |
| `0x082F5C` | `can_mailbox_data_copy` | can | comm.c | low | Copies data words from a source buffer (XAR5) into a destination pointer stored in a CAN s |
| `0x082F79` | `sci_port_configure_rx` | sci | comm.c | medium | Conditionally enables RXENA (SCICTL1 bit5) and TX/RX FIFO interrupt flags in a SCI port re |
| `0x082F95` | `clock_config_xtal_pll_div` | clock | clock.c | medium | Configures the crystal oscillator and PLL clock divider. EALLOW-protected: clears bit0 and |
| `0x082FB1` | `timer_channel_register` | timer | pwm.c | high | Registers a software timer channel. Takes channel index (0-15) in AR5, a 32-bit period val |
| `0x082FCD` | `sci_port_disable_selected` | sci | comm.c | medium | Clears selected SCI enable bits in a register block pointed to by ACC, controlled by a 5-b |
| `0x082FE7` | `sci_port_enable_selected` | sci | comm.c | medium | Sets selected SCI enable bits in a register block pointed to by ACC, controlled by a 5-bit |
| `0x083001` | `gpio_mux_write_csel2` | gpio | gpio.c | low | EALLOW-protected GPIO signal-mux write for the upper GPACSEL2 pair (offsets 24-25 of a GPI |
| `0x083019` | `gpio_mux_write_csel1` | gpio | gpio.c | low | EALLOW-protected GPIO signal-mux write for the lower GPACSEL1 pair (offsets 22-23 of a GPI |
| `0x083031` | `sci_register_timer_channels` | sci | comm.c | high | Allocates two software timer channels for SCI timing and registers them. Calls sub_82DCF t |
| `0x083049` | `pie_vect_table_init` | isr | foc.c | high | Initialises the PIE (Peripheral Interrupt Expansion) vector table under EALLOW. Fills 220  |
| `0x083077` | `eqep_write_config_regs` | eqep | pwm.c | medium | Writes three configuration words to an eQEP (quadrature encoder) peripheral register block |
| `0x08308D` | `timer_channel_deactivate` | timer | pwm.c | medium | Searches a software-timer channel table (base 0x0C5EA, 8 slots, stride 13 words) for a slo |
| `0x0830A3` | `rom_api_call_wrapper` | util | util.c | medium | Saves a minimal register context (ACC, DP, XAR0, XAR2, XAR3, XAR4, XAR5), resets the produ |
| `0x0830B7` | `sci_tx_buf_pop` | sci | comm.c | high | Pops one byte from the SCI TX circular buffer and writes it to the destination register po |
| `0x0830CB` | `sci_tx_interrupt_enable` | sci | comm.c | medium | Sets up the SCI TX interrupt for a SCI driver object (passed in XAR4, saved to XAR1). Stor |
| `0x0830EF` | `timer_channel_is_free` | timer | pwm.c | high | Checks whether a software timer channel slot is free (handler/period == 0). Input channel  |
| `0x083100` | `crc16_compute_buf` | comm | comm.c | medium | Computes CRC-16/ARC over a buffer of 'count' words. ACC=count, XAR4=buffer base pointer, X |
| `0x08310F` | `can_dispatch_pending_queue` | can | comm.c | medium | Drains the pending CAN/SCI message queue. Reads the 32-bit pending-item count from word_C0 |
| `0x08312B` | `crc16_step_byte` | comm | comm.c | high | Single-byte step of the CRC-16/ARC (LSB-first, polynomial 0xA001). XORs data byte (AH) int |
| `0x083143` | `app_main_trampoline` | startup | main.c | high | Boot trampoline that unconditionally calls the application main-code entry point (loc_8065 |
| `0x08314F` | `can_channel_bitmap_clear` | can | comm.c | medium | Clears the two active-channel bitmaps (byte_C33D and byte_C33E in GSx RAM) and zeroes 2 co |
| `0x083159` | `epwm_tb_prd_mode_set` | pwm | pwm.c | medium | Writes a 2-bit field into bits[15:14] of a peripheral register word (pointed to by ACC). C |
| `0x083173` | `codestart_0` | startup | main.c | high | C28x reset vector entry point (the 'codestart' boot stub). With EALLOW active: writes 0x68 |
| `0x08317B` | `sci_port_init_enable_int` | sci | comm.c | medium | Loads the SCI port configuration sub-object from XAR4[26] (offset 52 words into the parent |
| `0x083182` | `epwm_dbctl_field_write` | pwm | pwm.c | medium | Writes a 2-bit field into bits[11:10] of the ePWM Dead-Band Control register (or similar w |
| `0x083193` | `can_txqueue_init` | can | comm.c | medium | Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into |
| `0x083198` | `can_rxqueue_init` | can | comm.c | medium | Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into |
| `0x08319D` | `can_filter_init` | can | comm.c | medium | Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into |
| `0x0831A2` | `can_mailbox_send` | can | comm.c | medium | Thin wrapper: loads a CAN/peripheral driver sub-object from XAR4[62] (word offset 124 into |
| `0x0831A7` | `epwm_phase_idx_clamp6` | pwm | pwm.c | medium | Validates that an ePWM phase/channel index (in AL) is >= 6. If AL < 6: clears XAR4 to null |
| `0x0831AB` | `epwm_module_idx_clamp10` | pwm | pwm.c | medium | Validates that an ePWM module index (in AL) is >= 10. If AL < 10: clears XAR4 to null (inv |
| `0x0831AF` | `check_array_min4_or_null` | util | util.c | high | Validates that the count in AL is at least 4. If AL >= 4 the pointer in XAR4 is returned u |
| `0x0831B7` | `check_array_min16_or_null` | util | util.c | high | Validates that the count in AL is at least 16. If AL >= 16 the pointer in XAR4 is returned |
| `0x0831BB` | `check_array_min8_or_null` | util | util.c | high | Validates that the count in AL is at least 8. If AL >= 8 the pointer in XAR4 is returned u |
| `0x0831BF` | `check_array_min4b_or_null` | util | util.c | high | Identical semantics to check_array_min4_or_null (threshold 4). A separate instance called  |
| `0x0831C3` | `check_array_min10_or_null` | util | util.c | high | Validates that the count in AL is at least 10. If AL >= 10 the pointer in XAR4 is returned |
| `0x0831C7` | `clear_struct_fields6_7` | util | util.c | high | Zeroes words at offsets [6] and [7] of the struct pointed to by XAR4, then returns XAR4 un |
| `0x0831CD` | `cpu_mode_init` | startup | main.c | high | Initialises C28x CPU operating mode: clears the DBGM bit in ST1 (allowing the debug interf |
| `0x0831D0` | `store_acc32_to_struct_offset2` | util | util.c | medium | Stores the 32-bit value in ACC to word offset [2] (i.e. bytes 4-7) of the struct pointed t |
| `0x0831D2` | `store_acc32_to_struct_offset4` | util | util.c | medium | Stores the 32-bit value in ACC to word offset [4] of the struct pointed to by XAR4. Called |
| `0x0831D4` | `store_acc32_to_struct_base` | util | util.c | high | Stores the 32-bit value in ACC to word offset [0] (base) of the struct pointed to by XAR4. |
| `0x0831DA` | `init_check_always_ok` | startup | main.c | high | Returns 1 (true) unconditionally. Called at 0x8306D inside the C-runtime startup sequence  |
| `0x0831DC` | `null_callback` | util | util.c | high | Empty no-op function (single lretr). Called at 0x82CB0 as the terminal callback in sub_82C |
