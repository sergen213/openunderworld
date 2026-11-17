#!/usr/bin/env python3
"""Inventory PE files of the original game directory (read-only).

Usage: pe_inventory.py [--json OUT.json] [--imports-tsv OUT.tsv] FILE...

Prints a Markdown fact sheet per PE: headers, sections with entropy,
import/export counts, relocations, version resource, TLS, load config,
debug directory, Rich header (decoded comp.id -> tool build), overlay,
Authenticode presence and packer/DRM heuristics.  Requires `pefile`.
--imports-tsv writes every import of the given files (file, dll, ordinal,
name, IAT VA). Nothing is written next to the inputs.
"""
import argparse
import hashlib
import json
import math
import os
import sys
import time

import pefile

# Rich header product ids (high word of comp.id) -> toolchain, after
# "Microsoft Rich header" tables (dishather/richprint comp_id.txt).
RICH_PROD = {
    0x0001: "Import0 (linker-synthesised import stubs)",
    0x0002: "Linker510", 0x0004: "Linker600", 0x0006: "Cvtres500",
    0x000A: "Utc12_C (VC6)", 0x000B: "Utc12_CPP (VC6)", 0x000E: "Masm613",
    0x0013: "Linker512", 0x0019: "Implib700 (VS2002/2003 import lib)",
    0x005A: "Linker710p (VS2003)", 0x005D: "Implib710p", 0x005E: "Cvtres710p",
    0x005F: "Utc1310_C (VS2003)", 0x0060: "Utc1310_CPP (VS2003)",
    0x006D: "Utc1400_C (VS2005)", 0x006E: "Utc1400_CPP (VS2005)",
    0x0078: "Linker800 (VS2005)", 0x007A: "Implib800 (VS2005)",
    0x007B: "Masm800 (VS2005)", 0x007C: "Cvtres800 (VS2005)",
    0x0083: "Utc1500_C (VS2008)", 0x0084: "Utc1500_CPP (VS2008)",
    0x0091: "Linker900 (VS2008)", 0x0092: "Export900", 0x0093: "Implib900",
    0x0094: "Cvtres900", 0x0095: "Masm900",
    0x009A: "Utc1600_C (VS2010)", 0x009B: "Utc1600_CPP (VS2010)",
    0x009D: "Linker1000 (VS2010)", 0x009E: "Masm1000",
    0x00FF: "Cvtres1400+ (VS2015+)",
    0x0102: "Linker1400 (VS2015+)", 0x0103: "Masm1400", 0x0104: "Utc1900_C (VS2015+)",
    0x0105: "Utc1900_CPP (VS2015+)", 0x0101: "Implib1400 (VS2015+)",
    0x0106: "Utc1900_C_Std", 0x0107: "Utc1900_CPP_Std", 0x0108: "Utc1900_CVTCIL_C",
    0x0109: "Utc1900_CVTCIL_CPP", 0x010A: "Utc1900_LTCG_C", 0x010B: "Utc1900_LTCG_CPP",
    0x0100: "Export1400 (VS2015+)",
}

SCN_FLAGS = [(0x20000000, "X"), (0x40000000, "R"), (0x80000000, "W")]
PACKER_SECTIONS = {".securom", ".sdata_", ".dsstext", ".ccg", "UPX0", "UPX1",
                   ".aspack", ".adata", ".petite", ".pec", ".enigma1", ".vmp0",
                   ".vmp1", ".themida", ".nsp0", ".mpress1", ".stxt774", ".stxt371"}
DRM_STRINGS = [b"SecuROM", b"securom", b"StarForce", b"protect.dll", b"tages",
               b"SolidShield", b"steam_api", b"SteamAPI", b"paul.dll", b"drm"]


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def perms(ch: int) -> str:
    return "".join(f if ch & bit else "-" for bit, f in
                   [(0x40000000, "R"), (0x80000000, "W"), (0x20000000, "X")])


def rich_info(pe):
    rh = pe.parse_rich_header()
    if not rh:
        return None
    vals = rh["values"]
    out = []
    for i in range(0, len(vals), 2):
        compid, count = vals[i], vals[i + 1]
        prod, build = compid >> 16, compid & 0xFFFF
        out.append({"prod": prod, "build": build, "count": count,
                    "tool": RICH_PROD.get(prod, f"prod 0x{prod:04x}")})
    return {"key": rh["key"].hex() if isinstance(rh["key"], bytes) else rh["key"],
            "entries": out,
            "checksum_ok": rh.get("checksum") is not None}


def version_strings(pe):
    out = {}
    for fi in getattr(pe, "FileInfo", []) or []:
        for entry in fi:
            if getattr(entry, "Key", b"") == b"StringFileInfo":
                for st in entry.StringTable:
                    for k, v in st.entries.items():
                        out[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    if hasattr(pe, "VS_FIXEDFILEINFO") and pe.VS_FIXEDFILEINFO:
        f = pe.VS_FIXEDFILEINFO[0]
        out["_FixedFileVersion"] = "%d.%d.%d.%d" % (
            f.FileVersionMS >> 16, f.FileVersionMS & 0xFFFF,
            f.FileVersionLS >> 16, f.FileVersionLS & 0xFFFF)
        out["_FixedProductVersion"] = "%d.%d.%d.%d" % (
            f.ProductVersionMS >> 16, f.ProductVersionMS & 0xFFFF,
            f.ProductVersionLS >> 16, f.ProductVersionLS & 0xFFFF)
    return out


def resource_summary(pe):
    res = {}
    if not hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        return res
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        tname = pefile.RESOURCE_TYPE.get(t.id, str(t.name) if t.name else t.id)
        n, size = 0, 0
        for nm in getattr(t, "directory", None).entries if hasattr(t, "directory") else []:
            for lang in getattr(nm, "directory", None).entries if hasattr(nm, "directory") else []:
                n += 1
                size += lang.data.struct.Size
        res[str(tname)] = {"count": n, "bytes": size}
    return res


def inspect(path, imports_rows):
    data = open(path, "rb").read()
    pe = pefile.PE(data=data, fast_load=False)
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    info = {
        "file": os.path.basename(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "machine": hex(fh.Machine),
        "format": "PE32+" if oh.Magic == 0x20B else "PE32",
        "characteristics": hex(fh.Characteristics),
        "is_dll": bool(fh.Characteristics & 0x2000),
        "timestamp": fh.TimeDateStamp,
        "timestamp_utc": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(fh.TimeDateStamp)),
        "linker": f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}",
        "os_version": f"{oh.MajorOperatingSystemVersion}.{oh.MinorOperatingSystemVersion}",
        "subsystem": pefile.SUBSYSTEM_TYPE.get(oh.Subsystem, oh.Subsystem),
        "subsystem_version": f"{oh.MajorSubsystemVersion}.{oh.MinorSubsystemVersion}",
        "image_base": hex(oh.ImageBase),
        "entry_rva": hex(oh.AddressOfEntryPoint),
        "entry_va": hex(oh.ImageBase + oh.AddressOfEntryPoint),
        "size_of_image": hex(oh.SizeOfImage),
        "dll_characteristics": hex(oh.DllCharacteristics),
        "checksum_header": hex(oh.CheckSum),
        "checksum_computed": hex(pe.generate_checksum()),
        "stack_reserve": hex(oh.SizeOfStackReserve),
        "heap_reserve": hex(oh.SizeOfHeapReserve),
    }
    entry_section = None
    secs = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("latin-1")
        raw = s.get_data()
        secs.append({"name": name, "va": hex(oh.ImageBase + s.VirtualAddress),
                     "vsize": hex(s.Misc_VirtualSize), "raw": hex(s.SizeOfRawData),
                     "rawptr": hex(s.PointerToRawData),
                     "perm": perms(s.Characteristics), "char": hex(s.Characteristics),
                     "entropy": round(entropy(raw[:s.SizeOfRawData]), 3)})
        if s.contains_rva(oh.AddressOfEntryPoint):
            entry_section = name
    info["sections"] = secs
    info["entry_section"] = entry_section

    imps = {}
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = d.dll.decode("latin-1")
        imps[dll] = imps.get(dll, 0) + len(d.imports)
        for i in d.imports:
            imports_rows.append((info["file"], dll, i.ordinal if i.import_by_ordinal else "",
                                 (i.name or b"").decode("latin-1"), hex(i.address)))
    info["imports"] = imps
    dimps = {}
    for d in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        dimps[d.dll.decode("latin-1")] = len(d.imports)
    info["delay_imports"] = dimps
    exps = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        e = pe.DIRECTORY_ENTRY_EXPORT
        info["export_dll_name"] = pe.get_string_at_rva(e.struct.Name).decode("latin-1")
        for s in e.symbols:
            exps.append({"ord": s.ordinal, "name": (s.name or b"").decode("latin-1"),
                         "rva": hex(s.address),
                         "forwarder": (s.forwarder or b"").decode("latin-1")})
    info["exports"] = exps

    dd = oh.DATA_DIRECTORY
    info["relocations"] = dd[5].Size > 0 and not (fh.Characteristics & 0x1)
    info["reloc_dir_size"] = dd[5].Size
    info["relocs_stripped_flag"] = bool(fh.Characteristics & 0x1)
    info["resources"] = resource_summary(pe)
    info["version"] = version_strings(pe)

    tls = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    if tls:
        cbs = []
        va = tls.struct.AddressOfCallBacks
        if va:
            rva = va - oh.ImageBase
            for k in range(64):
                try:
                    v = pe.get_dword_at_rva(rva + 4 * k)
                except Exception:
                    break
                if not v:
                    break
                cbs.append(hex(v))
        info["tls"] = {"callbacks_ptr": hex(va), "callbacks": cbs}
    else:
        info["tls"] = None

    lc = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if lc:
        st = lc.struct
        info["load_config"] = {
            "size": st.Size,
            "security_cookie": hex(getattr(st, "SecurityCookie", 0)),
            "se_handler_table": hex(getattr(st, "SEHandlerTable", 0)),
            "se_handler_count": getattr(st, "SEHandlerCount", 0),
            "guard_flags": hex(getattr(st, "GuardFlags", 0) or 0),
        }
    else:
        info["load_config"] = None

    dbg = []
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        item = {"type": pefile.DEBUG_TYPE.get(d.struct.Type, d.struct.Type),
                "timestamp": d.struct.TimeDateStamp}
        e = getattr(d, "entry", None)
        if e is not None and hasattr(e, "PdbFileName"):
            item["pdb"] = e.PdbFileName.rstrip(b"\0").decode("latin-1")
            item["age"] = getattr(e, "Age", None)
            if hasattr(e, "Signature_Data1"):
                item["guid"] = "%08x-%04x-%04x-%s" % (
                    e.Signature_Data1, e.Signature_Data2, e.Signature_Data3,
                    bytes([e.Signature_Data4, e.Signature_Data5]).hex() +
                    "-" + e.Signature_Data6.hex())
        dbg.append(item)
    info["debug"] = dbg
    info["rich"] = rich_info(pe)

    ov = pe.get_overlay_data_start_offset()
    info["overlay"] = None if ov is None else {"offset": hex(ov), "size": len(data) - ov,
                                               "entropy": round(entropy(data[ov:]), 3),
                                               "head": data[ov:ov + 16].hex()}
    sec = dd[4]
    info["authenticode"] = {"offset": hex(sec.VirtualAddress), "size": sec.Size} if sec.Size else None
    info["clr"] = dd[14].Size > 0
    info["packer_sections"] = [s["name"] for s in secs if s["name"] in PACKER_SECTIONS]
    info["drm_strings"] = {s.decode(): data.count(s) for s in DRM_STRINGS if data.count(s)}
    return info


def md(info):
    o = [f"### {info['file']}", "", "| Field | Value |", "|---|---|"]
    v = info["version"]
    rows = [
        ("Size / SHA-256", f"{info['size']} / `{info['sha256']}`"),
        ("Machine / format", f"{info['machine']} / {info['format']} {'DLL' if info['is_dll'] else 'EXE'}"),
        ("TimeDateStamp", f"0x{info['timestamp']:08x} = {info['timestamp_utc']} UTC"),
        ("Linker / OS / subsystem", f"{info['linker']} / {info['os_version']} / {info['subsystem']} {info['subsystem_version']}"),
        ("ImageBase / entry RVA / entry VA", f"{info['image_base']} / {info['entry_rva']} / {info['entry_va']} (`{info['entry_section']}`)"),
        ("DllCharacteristics", info["dll_characteristics"]),
        ("Header checksum / computed", f"{info['checksum_header']} / {info['checksum_computed']}"),
        ("Relocations", f"{'yes' if info['relocations'] else 'no'} (dir size {info['reloc_dir_size']}, stripped flag {info['relocs_stripped_flag']})"),
        ("TLS", json.dumps(info["tls"]) if info["tls"] else "none"),
        ("Load config", json.dumps(info["load_config"]) if info["load_config"] else "none"),
        ("Debug", json.dumps(info["debug"]) if info["debug"] else "none"),
        ("Overlay", json.dumps(info["overlay"]) if info["overlay"] else "none"),
        ("Authenticode (security dir)", json.dumps(info["authenticode"]) if info["authenticode"] else "none"),
        ("Packer section names / DRM strings", f"{info['packer_sections'] or 'none'} / {info['drm_strings'] or 'none'}"),
        ("Version", "; ".join(f"{k}={val}" for k, val in v.items()) or "none"),
        ("Resources", ", ".join(f"{k}:{x['count']}({x['bytes']} B)" for k, x in info["resources"].items()) or "none"),
        ("Imports", ", ".join(f"{k} {n}" for k, n in info["imports"].items()) or "none"),
        ("Delay imports", ", ".join(f"{k} {n}" for k, n in info["delay_imports"].items()) or "none"),
        ("Exports", f"{len(info['exports'])}" + (f" (`{info.get('export_dll_name')}`)" if info["exports"] else "")),
    ]
    for k, val in rows:
        o.append(f"| {k} | {str(val).replace('|', '/')} |")
    o += ["", "| Section | VA | VSize | Raw | Perm | Entropy |", "|---|---|---|---|---|---|"]
    for s in info["sections"]:
        o.append(f"| `{s['name']}` | {s['va']} | {s['vsize']} | {s['raw']} | {s['perm']} | {s['entropy']} |")
    if info["rich"]:
        o += ["", "Rich header (prod.build × count):", ""]
        o.append(", ".join(f"{e['tool']} b{e['build']}×{e['count']}" for e in info["rich"]["entries"]))
    else:
        o += ["", "Rich header: none"]
    o.append("")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--imports-tsv")
    a = ap.parse_args()
    rows, infos = [], []
    for f in a.files:
        try:
            infos.append(inspect(f, rows))
        except pefile.PEFormatError as e:
            print(f"{f}: not a PE ({e})", file=sys.stderr)
    for i in infos:
        print(md(i))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(infos, fh, indent=1)
    if a.imports_tsv:
        with open(a.imports_tsv, "w") as fh:
            fh.write("file\tdll\tordinal\tname\tiat_va\n")
            for r in rows:
                fh.write("\t".join(str(x) for x in r) + "\n")


if __name__ == "__main__":
    main()
