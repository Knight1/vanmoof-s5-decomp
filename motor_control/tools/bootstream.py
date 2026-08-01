#!/usr/bin/env python3
"""
bootstream.py — TI C28x boot-table parser / flash-region extractor for the
VanMoof S5 `motor_control` firmware (TI TMS320F280049C, C2000/C28x).

The image is a raw TI C28x serial-boot table (boot key 0x08AA = 16-bit data
stream): a key word, 8 reserved words, a 2-word entry point, then
`{size, dest_MSW, dest_LSW, data[size]}` blocks terminated by a zero size. C28x
is word-addressed (one 16-bit word per address); each region is emitted raw
little-endian, 2 bytes per word, named by its load word-address — the exact
input an IDA `tms32028` (or a Ghidra C28x) loader expects.

This is the container-level analysis step (no C28x disassembler is required to
run it); it mirrors the S3 `motorware/tools/bootstream.py` pipeline.

Usage:
    bootstream.py <image.bin>                 # report (key/entry/segment map)
    bootstream.py --extract=<dir> <image.bin> # write region_*.bin + manifest.json
"""
import sys, os, json

def parse(data):
    w = [data[i] | (data[i + 1] << 8) for i in range(0, len(data) & ~1, 2)]
    key = w[0]
    reserved = w[1:9]
    entry = (w[9] << 16) | w[10]          # MSW first
    idx, blocks = 11, []
    while idx < len(w):
        size = w[idx]; idx += 1
        if size == 0:
            break
        dest = (w[idx] << 16) | w[idx + 1]; idx += 2
        start_byte = idx * 2
        blocks.append({"dest": dest, "size_words": size,
                       "file_off": start_byte, "nbytes": size * 2})
        idx += size
    return {"key": key, "reserved": reserved, "entry": entry,
            "blocks": blocks, "words_consumed": idx}

def region(a):                            # F28004x (F280049C) memory map
    if a < 0x800:        return "M0 RAM"
    if a < 0xC00:        return "M1 RAM"
    if 0x8000 <= a < 0xC000:  return "LSx RAM"
    if 0xC000 <= a < 0x1C000: return "GSx RAM"
    if 0x80000 <= a < 0xC0000: return "FLASH"
    return "?"

def main():
    args = [a for a in sys.argv[1:]]
    extract = None
    for a in list(args):
        if a.startswith("--extract="):
            extract = a.split("=", 1)[1]; args.remove(a)
    if not args:
        print(__doc__); sys.exit(2)
    path = args[0]
    data = open(path, "rb").read()
    info = parse(data)

    print(f"file: {path}")
    print(f"bytes: {len(data)}  words: {len(data)//2}")
    print(f"boot key: 0x{info['key']:04X}  (0x08AA = TI C28x 16-bit boot stream)")
    print(f"reserved[1..8]: {' '.join('%04X'%x for x in info['reserved'])}")
    print(f"entry point: 0x{info['entry']:06X}")
    print("segments:")
    for b in info["blocks"]:
        print(f"  dest 0x{b['dest']:06X}  {b['size_words']:6d} words"
              f"  ({b['nbytes']} B)  [{region(b['dest'])}]")
    print(f"words consumed: {info['words_consumed']} of {len(data)//2}")

    if extract:
        os.makedirs(extract, exist_ok=True)
        manifest = {"image": os.path.basename(path),
                    "mcu": "TMS320F280049C (C2000/C28x)",
                    "word_addressed": True, "bytes_per_word": 2,
                    "boot_key": f"0x{info['key']:04X}",
                    "entry": f"0x{info['entry']:06X}", "regions": []}
        for b in info["blocks"]:
            name = f"region_{b['dest']:06X}.bin"
            with open(os.path.join(extract, name), "wb") as f:
                f.write(data[b["file_off"]:b["file_off"] + b["nbytes"]])
            manifest["regions"].append(
                {"file": name, "load_word_addr": f"0x{b['dest']:06X}",
                 "size_words": b["size_words"], "region": region(b["dest"])})
        with open(os.path.join(extract, "manifest.json"), "w") as f:
            json.dump(manifest, f, indent=2)
        print(f"\nextracted {len(info['blocks'])} regions + manifest.json -> {extract}/")

if __name__ == "__main__":
    main()
