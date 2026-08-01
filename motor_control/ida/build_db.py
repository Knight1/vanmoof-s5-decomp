# -*- coding: utf-8 -*-
# Build a motor_control IDA database for the TMS320F280049C (C2000 / C28x).
#
# Adapted from the S3 motorware ida/build_db.py for the F28004x family. Launched
# on region_08065C.bin (the 11137-word application region) with -ptms32028 so
# IDA's binary loader uses the C28x 16-bit addressable unit (1 IDA "byte" = one
# 16-bit word). This script then:
#   * rebases the auto-loaded main region to its true word address 0x08065C,
#   * loads the remaining reconstructed flash regions at their word addresses,
#   * defines RAM (M0/M1, LSx, GSx) + a peripheral-frame segment,
#   * labels the stable C28x core MMIO (PIE / CPU timers); device-specific
#     EPWM/ADC/eQEP/CAN labels are a refine pass once touched addresses show,
#   * marks the boot-stream entry (0x080000 = codestart LB to _c_int00),
#   * runs auto-analysis and exports a listing + a function inventory JSON.
#
# argv: <image_dir> <out_dir>   (mixed C:/ paths; the dir holding region_*.bin)
from __future__ import print_function
import json, os, struct
import idaapi, idc, ida_auto, ida_bytes, ida_segment, ida_name, ida_entry, ida_funcs, ida_pro

ARGV = idc.ARGV
IMAGE_DIR = ARGV[1] if len(ARGV) > 1 else "."
OUT_DIR = ARGV[2] if len(ARGV) > 2 else "."
MAIN_BASE = 0x08065C    # the auto-loaded region (largest / app)

LOG = open(os.path.join(OUT_DIR, "build_db.log"), "w")
def log(s):
    LOG.write(str(s) + "\n"); LOG.flush()

# F28004x (F280049C) RAM map (word addresses, data space).
RAM_SEGS = [
    (0x000000, 0x000400, "M0",  "DATA"),
    (0x000400, 0x000800, "M1",  "DATA"),
    (0x008000, 0x00C000, "LSx", "DATA"),   # LS0-LS7
    (0x00C000, 0x01C000, "GSx", "DATA"),   # GS0-GS15
]
PERIPH_SEG  = (0x000800, 0x008000, "PERIPH", "DATA")   # CPU peripheral frames
PERIPH_SEG2 = (0x040000, 0x060000, "PERIPH2", "DATA")  # EPWM/eCAP/eQEP/SCI/SPI/CAN frames

# Stable C28x core MMIO (same across the family).
MMIO = {
    0x000C00: "CpuTimer0Regs", 0x000C08: "CpuTimer1Regs", 0x000C10: "CpuTimer2Regs",
    0x000CE0: "PieCtrlRegs",   0x000D00: "PieVectTable",
    0x000B00: "AdcaResultRegs",0x000B20: "AdcbResultRegs",0x000B40: "AdccResultRegs",
    0x001000: "DmaRegs",       0x001400: "Cla1Regs",
}

def ensure_seg(start, end, name, sclass):
    if ida_segment.getseg(start) is None:
        idaapi.add_segm(0, start, end, name, sclass)
    if ida_segment.getseg(start):
        idc.set_segm_name(start, name)

def load_region(fn, base):
    data = open(os.path.join(IMAGE_DIR, fn), "rb").read()
    nwords = len(data) // 2
    ensure_seg(base, base + nwords, "FLASH_%06X" % base, "CODE")
    for i in range(nwords):
        ida_bytes.patch_byte(base + i, struct.unpack_from("<H", data, 2 * i)[0])
    log("loaded %s -> 0x%06X..0x%06X (%d words)" % (fn, base, base + nwords, nwords))

def main():
    log("ARGV=%r proc=%s" % (ARGV, idaapi.get_idp_name()))

    # 1) rebase the auto-loaded main region to its true word address.
    seg0 = ida_segment.getnseg(0)
    delta = MAIN_BASE - seg0.start_ea
    if delta:
        idaapi.rebase_program(delta, idaapi.MSF_FIXONCE)
        log("rebased main -> 0x%X" % ida_segment.getnseg(0).start_ea)
    idc.set_segm_name(MAIN_BASE, "FLASH_%06X" % MAIN_BASE)

    # 2) load the remaining regions at their word addresses.
    man = json.load(open(os.path.join(IMAGE_DIR, "manifest.json")))
    placed = {MAIN_BASE}
    for r in man["regions"]:
        base = int(r["load_word_addr"], 16)
        if base not in placed:
            load_region(r["file"], base); placed.add(base)
    entry = int(man["entry"], 16)

    # 3) RAM + peripheral segments + stable MMIO labels.
    for s in RAM_SEGS: ensure_seg(*s)
    ensure_seg(*PERIPH_SEG); ensure_seg(*PERIPH_SEG2)
    for ea, nm in MMIO.items():
        ida_name.set_name(ea, nm, ida_name.SN_NOCHECK | ida_name.SN_FORCE)

    # 4) entry: the codestart branch at 0x080000 -> _c_int00.
    ida_entry.add_entry(entry, entry, "codestart", 1)
    idc.create_insn(entry)
    ida_funcs.add_func(entry)

    # 5) mark the flash regions executable, force a linear code sweep over the
    #    RTS (region_080004) and the app (region_08065C), then form functions at
    #    every call target. C28x auto-analysis does not propagate from the entry
    #    into raw flash on its own.
    for s_ea in (0x080000, 0x080004, 0x08065C, 0x0831E0):
        sg = ida_segment.getseg(s_ea)
        if sg:
            idc.set_segm_class(s_ea, "CODE")
            sg.perm = 5   # R+X

    def force_code(a, b):
        n = 0
        while a < b:
            if ida_bytes.is_code(ida_bytes.get_full_flags(a)):
                nh = idc.next_head(a, b)
                a = nh if nh != idc.BADADDR else a + 1
                continue
            ln = idc.create_insn(a)
            if ln > 0:
                n += 1; a += ln
            else:
                a += 1
        return n

    APP = (0x08065C, 0x0831DD)       # 11137-word application
    RTS = (0x080004, 0x08065A)       # C runtime init
    nins = force_code(*RTS) + force_code(*APP)
    ida_auto.auto_wait()

    # form functions at LCR/CALL targets that don't yet have one.
    import idautils
    made = 0
    for lo, hi in (RTS, APP):
        ea = lo
        while ea != idc.BADADDR and ea < hi:
            for xr in idautils.XrefsFrom(ea, 0):
                if xr.type in (idaapi.fl_CN, idaapi.fl_CF) and 0x080000 <= xr.to < 0x084000:
                    if idaapi.get_func(xr.to) is None and ida_funcs.add_func(xr.to):
                        made += 1
            ea = idc.next_head(ea, hi)
    ida_funcs.add_func(entry)
    ida_auto.auto_wait()
    log("force-coded %d insns, added %d funcs" % (nins, made))

    # 6) export the full listing.
    idc.gen_file(idc.OFILE_LST, os.path.join(OUT_DIR, "motor_control.lst"), 0, idc.BADADDR, 0)

    # 7) function inventory + call graph + the MMIO addresses each fn touches.
    import idautils
    funcs = {}
    mmio_hits = {}
    for fea in idautils.Functions():
        items = list(idautils.FuncItems(fea))
        callees = set()
        touched = set()
        for ea in items:
            for xr in idautils.XrefsFrom(ea, 0):
                if xr.type in (idaapi.fl_CN, idaapi.fl_CF):
                    if idc.get_func_name(xr.to):
                        callees.add(xr.to)
                elif xr.to < 0x080000:                 # data ref below flash = MMIO/RAM
                    touched.add(xr.to)
                    mmio_hits[xr.to] = mmio_hits.get(xr.to, 0) + 1
        callers = set(xr.frm for xr in idautils.XrefsTo(fea, 0)
                      if xr.type in (idaapi.fl_CN, idaapi.fl_CF))
        funcs["0x%06X" % fea] = {
            "name": idc.get_func_name(fea), "size": len(items),
            "callees": sorted("0x%06X" % c for c in callees),
            "ncallers": len(callers),
            "data_refs": sorted("0x%06X" % t for t in touched)[:40],
        }
    json.dump(funcs, open(os.path.join(OUT_DIR, "funcmap.json"), "w"), indent=1)
    json.dump({"0x%06X" % a: n for a, n in sorted(mmio_hits.items())},
              open(os.path.join(OUT_DIR, "mmio_hits.json"), "w"), indent=1)

    # 8) compact per-function disassembly (the translation artifact).
    with open(os.path.join(OUT_DIR, "functions.asm"), "w") as fh:
        for k in sorted(funcs, key=lambda x: int(x, 16)):
            v = funcs[k]; fea = int(k, 16)
            fh.write("\n; ===== %s @0x%06X  size=%d  callers=%d  callees=%s =====\n"
                     % (v["name"], fea, v["size"], v["ncallers"], ",".join(v["callees"])))
            for ea in idautils.FuncItems(fea):
                fh.write("%06X  %s\n" % (ea, idc.generate_disasm_line(ea, 0)))
    log("functions found: %d  distinct data/MMIO addrs: %d" % (len(funcs), len(mmio_hits)))
    log("DONE")

try:
    main()
except Exception as e:
    import traceback
    log("FATAL: %r\n%s" % (e, traceback.format_exc()))
finally:
    LOG.close()
    idc.save_database(os.path.join(OUT_DIR, "motor_control.idb"))
    ida_pro.qexit(0)
