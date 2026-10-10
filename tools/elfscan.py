import sys, json, re, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.sections import SymbolTableSection

so, out = sys.argv[1], sys.argv[2]
f = open(so, "rb"); e = ELFFile(f)
info = {"class": e.elfclass, "machine": e["e_machine"], "type": e["e_type"], "needed": [], "soname": None,
        "exports": [], "imports": [], "strings": []}
for s in e.iter_sections():
    if isinstance(s, DynamicSection):
        for t in s.iter_tags():
            if t.entry.d_tag == "DT_NEEDED": info["needed"].append(t.needed)
            elif t.entry.d_tag == "DT_SONAME": info["soname"] = t.soname
    if s.name == ".dynsym" and isinstance(s, SymbolTableSection):
        for sym in s.iter_symbols():
            if not sym.name: continue
            b, ty = sym["st_info"]["bind"], sym["st_info"]["type"]
            if sym["st_shndx"] == "SHN_UNDEF":
                info["imports"].append([sym.name, b, ty])
            elif b in ("STB_GLOBAL", "STB_WEAK"):
                info["exports"].append([sym.name, b, ty, sym["st_size"]])
for s in e.iter_sections():
    if s.name in (".rodata", ".rodata.str1.1", ".rodata.str1.4", ".rodata.str1.8", ".data.rel.ro", ".data"):
        for m in re.finditer(rb"[\x20-\x7e]{3,}", s.data()):
            info["strings"].append([s.name, m.group().decode()])
json.dump(info, open(out, "w"), indent=0)

ex = info["exports"]; im = info["imports"]
jni = [x for x in ex if x[0].startswith("Java_") or x[0].startswith("JNI_")]
print(f"[elf] {so.rsplit('/',1)[-1]} class={info['class']} machine={info['machine']} type={info['type']} soname={info['soname']}")
print(f"[elf] needed={info['needed']}")
print(f"[elf] exports={len(ex)} (jni={len(jni)}, other={len(ex)-len(jni)}) imports={len(im)} strings={len(info['strings'])}")
