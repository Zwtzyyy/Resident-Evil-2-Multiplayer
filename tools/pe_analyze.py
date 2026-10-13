#!/usr/bin/env python3
"""Minimal, dependency-free PE analyzer for the re2coop helper DLLs.

Prints sections, data directories, exports, imports and ASCII/UTF-16 strings so
we can reconstruct what each plugin does without a disassembler.
"""
import struct
import sys


def rva_to_off(rva, sections):
    for name, vsize, vaddr, rawsize, rawptr in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


def cstr(data, off):
    end = data.find(b"\x00", off)
    if end == -1:
        return ""
    return data[off:end].decode("latin-1", "replace")


def parse(path):
    with open(path, "rb") as fh:
        data = fh.read()
    out = {"path": path, "size": len(data)}
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("no PE signature")
    machine, nsec, _, _, _, optsize, chars = struct.unpack_from("<HHIIIHH", data, pe + 4)
    out["machine"] = hex(machine)
    out["is_dll"] = bool(chars & 0x2000)
    out["timestamp"] = struct.unpack_from("<I", data, pe + 8)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    out["magic"] = hex(magic)
    if magic == 0x20B:  # PE32+
        dd_off = opt + 112
    else:
        dd_off = opt + 96
    num_dd = struct.unpack_from("<I", data, opt + (108 if magic == 0x20B else 92))[0]
    dirs = []
    for i in range(num_dd):
        va, sz = struct.unpack_from("<II", data, dd_off + i * 8)
        dirs.append((i, va, sz))
    out["data_directories"] = [d for d in dirs if d[2] != 0]

    sections = []
    sec_off = opt + optsize
    for i in range(nsec):
        name = data[sec_off:sec_off + 8].rstrip(b"\x00").decode("latin-1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sec_off + 8)
        sections.append((name, vsize, vaddr, rawsize, rawptr))
        sec_off += 40
    out["sections"] = [{"name": s[0], "vaddr": hex(s[2]), "vsize": s[1],
                        "rawptr": hex(s[4]), "rawsize": s[3]} for s in sections]

    def dd(idx):
        return dirs[idx] if idx < len(dirs) else (idx, 0, 0)

    # Exports (dir 0)
    _, eva, esz = dd(0)
    exports = []
    if eva:
        eo = rva_to_off(eva, sections)
        if eo:
            (_, _, _, _, name_rva, base_ord, nfunc, nname, funcs_rva, names_rva,
             ords_rva) = struct.unpack_from("<IIHHIIIIIII", data, eo)
            dll_name = cstr(data, rva_to_off(name_rva, sections)) if name_rva else ""
            out["export_dll_name"] = dll_name
            for i in range(nname):
                noff = rva_to_off(names_rva, sections) + i * 4
                nrva = struct.unpack_from("<I", data, noff)[0]
                fname = cstr(data, rva_to_off(nrva, sections))
                ordi = struct.unpack_from("<H", data, rva_to_off(ords_rva, sections) + i * 2)[0]
                frva = struct.unpack_from("<I", data, rva_to_off(funcs_rva, sections) + ordi * 4)[0]
                exports.append({"name": fname, "ordinal": base_ord + ordi, "rva": hex(frva),
                                "off": rva_to_off(frva, sections)})
    out["exports"] = exports

    # Imports (dir 1)
    _, iva, isz = dd(1)
    imports = {}
    if iva:
        off = rva_to_off(iva, sections)
        while True:
            ilt, ts, fc, nrva, iat = struct.unpack_from("<IIIII", data, off)
            if ilt == 0 and nrva == 0:
                break
            dll = cstr(data, rva_to_off(nrva, sections))
            thunk = ilt or iat
            funcs = []
            toff = rva_to_off(thunk, sections)
            while True:
                val = struct.unpack_from("<Q" if magic == 0x20B else "<I", data, toff)[0]
                if val == 0:
                    break
                if magic == 0x20B and (val & (1 << 63)):
                    funcs.append(f"#{val & 0xFFFF}")
                else:
                    noff = rva_to_off(val & 0x7FFFFFFF, sections)
                    funcs.append(cstr(data, noff + 2))  # skip IMAGE_IMPORT_BY_NAME hint
                toff += 8 if magic == 0x20B else 4
            imports[dll] = funcs
            off += 20
    out["imports"] = imports

    # Strings from all sections
    def strings(minlen=5):
        res = set()
        for name, vsize, vaddr, rawsize, rawptr in sections:
            chunk = data[rawptr:rawptr + rawsize]
            cur = bytearray()
            for b in chunk:
                if 32 <= b < 127:
                    cur.append(b)
                else:
                    if len(cur) >= minlen:
                        res.add(cur.decode("latin-1"))
                    cur = bytearray()
            if len(cur) >= minlen:
                res.add(cur.decode("latin-1"))
        return sorted(res)
    out["strings"] = strings()

    def wide_strings(minlen=4):
        res = set()
        for name, vsize, vaddr, rawsize, rawptr in sections:
            chunk = data[rawptr:rawptr + rawsize]
            i = 0
            while i + 1 < len(chunk):
                if 32 <= chunk[i] < 127 and chunk[i + 1] == 0:
                    j = i
                    cur = bytearray()
                    while j + 1 < len(chunk) and 32 <= chunk[j] < 127 and chunk[j + 1] == 0:
                        cur.append(chunk[j])
                        j += 2
                    if len(cur) >= minlen:
                        res.add(cur.decode("latin-1"))
                    i = j
                else:
                    i += 1
        return sorted(res)
    out["wide_strings"] = wide_strings()
    return out


def main():
    import json
    for path in sys.argv[1:]:
        info = parse(path)
        print("=" * 80)
        print(f"FILE: {info['path']}  ({info['size']} bytes)  machine={info['machine']} dll={info['is_dll']}")
        print(f"export_dll_name: {info.get('export_dll_name')}")
        print("-- sections --")
        for s in info["sections"]:
            print(f"  {s['name']:<10} va={s['vaddr']:<10} vsize={s['vsize']:<8} raw={s['rawptr']:<10} rawsize={s['rawsize']}")
        print("-- data directories (nonzero) --")
        for i, va, sz in info["data_directories"]:
            print(f"  dir[{i}] rva=0x{va:x} size=0x{sz:x}")
        print("-- exports --")
        for e in info["exports"]:
            print(f"  {e['name']} (ord {e['ordinal']}) rva={e['rva']} off={e['off']}")
        print("-- imports --")
        for dll, funcs in info["imports"].items():
            print(f"  {dll}: {', '.join(funcs)}")
        print(f"-- strings ({len(info['strings'])}) --")
        for s in info["strings"]:
            print("  " + s)
        print(f"-- wide strings ({len(info['wide_strings'])}) --")
        for s in info["wide_strings"]:
            print("  " + s)
    print("=" * 80)


if __name__ == "__main__":
    main()
