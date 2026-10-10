import sys, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from elftools.elf.relocation import RelocationSection
import capstone as cs

so = sys.argv[1]; funcs = sys.argv[2:]
f = open(so, "rb"); e = ELFFile(f)
segs = [(s["p_vaddr"], s["p_offset"], s["p_filesz"]) for s in e.iter_segments() if s["p_type"] == "PT_LOAD"]
raw = open(so, "rb").read()

def rd(va, n):
    for v, o, sz in segs:
        if v <= va < v + sz:
            return raw[o + va - v:o + va - v + n]
    return None

def cstr(va):
    b = rd(va, 256)
    if not b: return None
    s = b.split(b"\0", 1)[0]
    return s.decode() if s and all(32 <= c < 127 for c in s) else None

syms = {}; byaddr = {}
for s in e.iter_sections():
    if isinstance(s, SymbolTableSection):
        for y in s.iter_symbols():
            if y.name and y["st_value"]:
                syms[y.name] = (y["st_value"], y["st_size"]); byaddr.setdefault(y["st_value"], y.name)
dsym = e.get_section_by_name(".dynsym")
got2name = {}
for s in e.iter_sections():
    if isinstance(s, RelocationSection):
        for r in s.iter_relocations():
            if r["r_info_sym"]:
                got2name[r["r_offset"]] = dsym.get_symbol(r["r_info_sym"]).name
plt = e.get_section_by_name(".plt")
md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM); md.detail = True
plt2name = {}
if plt:
    base = plt["sh_addr"]; data = plt.data()
    ins = list(md.disasm(data, base))
    for i in range(len(ins) - 1):
        a, b = ins[i], ins[i + 1]
        if a.mnemonic == "adrp" and b.mnemonic == "ldr":
            page = a.operands[1].imm; off = b.operands[1].mem.disp
            nm = got2name.get(page + off)
            if nm: plt2name[a.address] = nm

for fn in funcs:
    if fn not in syms:
        print(f"## {fn}: not found"); continue
    va, sz = syms[fn]
    code = rd(va, sz)
    regs = {}; out = []
    for i in md.disasm(code, va):
        m = i.mnemonic; ops = i.operands
        if m == "adrp":
            regs[ops[0].reg] = ops[1].imm
        elif m == "add" and len(ops) == 3 and ops[2].type == cs.arm64.ARM64_OP_IMM and ops[1].reg in regs:
            v = regs[ops[1].reg] + ops[2].imm; regs[ops[0].reg] = v
            s = cstr(v)
            if s: out.append(f"'{s}'")
            elif v in byaddr: out.append(f"&{byaddr[v]}")
        elif m == "ldr" and len(ops) == 2 and ops[1].type == cs.arm64.ARM64_OP_MEM and ops[1].mem.base in regs:
            v = regs[ops[1].mem.base] + ops[1].mem.disp
            nm = byaddr.get(v) or got2name.get(v)
            if nm: out.append(f"[{nm}]")
        elif m in ("bl", "b") and ops and ops[0].type == cs.arm64.ARM64_OP_IMM:
            t = ops[0].imm
            nm = plt2name.get(t) or byaddr.get(t)
            if nm: out.append(f"{'call' if m=='bl' else 'tail'} {nm}")
            elif m == "b": out.append(f"b→{t-va:+x}" if not (va <= t < va+sz) else f"j{t-va:x}")
        elif m == "blr":
            out.append("call*")
        elif m in ("cbz", "cbnz", "tbz", "tbnz") or m.startswith("b."):
            out.append(f"{m}@{i.address-va:x}")
        elif m == "ret":
            out.append("ret")
    print(f"## {fn} @0x{va:x} size={sz}")
    print("  " + " | ".join(out))
