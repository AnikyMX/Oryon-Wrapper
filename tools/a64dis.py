import sys, capstone as cs
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
e = ELFFile(open(sys.argv[1], "rb")); raw = open(sys.argv[1], "rb").read()
dsym = e.get_section_by_name(".dynsym")
syms = {y.name: (y["st_value"], y["st_size"]) for y in dsym.iter_symbols() if y.name}
byaddr = {v[0]: k for k, v in syms.items() if v[0]}
segs = [(s["p_vaddr"], s["p_offset"], s["p_filesz"]) for s in e.iter_segments() if s["p_type"] == "PT_LOAD"]
def rd(va, n):
    for v, o, sz in segs:
        if v <= va < v + sz: return raw[o+va-v:o+va-v+n]
def cstr(va):
    b = rd(va, 200)
    if not b: return None
    s = b.split(b"\0", 1)[0]
    return s.decode() if s and all(32 <= c < 127 for c in s) else None
got = {}
for s in e.iter_sections():
    if isinstance(s, RelocationSection):
        for r in s.iter_relocations():
            if r["r_info_sym"]: got[r["r_offset"]] = dsym.get_symbol(r["r_info_sym"]).name
md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM); md.detail = True
plt = e.get_section_by_name(".plt"); pins = list(md.disasm(plt.data(), plt["sh_addr"])); p2n = {}
for a, b in zip(pins, pins[1:]):
    if a.mnemonic == "adrp" and b.mnemonic == "ldr":
        n = got.get(a.operands[1].imm + b.operands[1].mem.disp)
        if n: p2n[a.address] = n
for fn in sys.argv[2:]:
    va, sz = syms[fn]; regs = {}
    print(f"## {fn} size={sz}")
    for ins in md.disasm(rd(va, sz), va):
        ann = ""; ops = ins.operands
        if ins.mnemonic == "adrp": regs[ops[0].reg] = ops[1].imm
        elif ins.mnemonic == "add" and len(ops) == 3 and ops[2].type == cs.arm64.ARM64_OP_IMM and ops[1].reg in regs:
            a = regs[ops[1].reg] + ops[2].imm; s = cstr(a); ann = f"  ; '{s}'" if s else (f"  ; &{byaddr[a]}" if a in byaddr else "")
        elif ins.mnemonic == "ldr" and len(ops) == 2 and ops[1].type == cs.arm64.ARM64_OP_MEM and ops[1].mem.base in regs:
            a = regs[ops[1].mem.base] + ops[1].mem.disp; n = byaddr.get(a) or got.get(a); ann = f"  ; [{n}]" if n else ""
        elif ins.mnemonic in ("bl", "b") and ops[0].type == cs.arm64.ARM64_OP_IMM:
            t = ops[0].imm; n = p2n.get(t) or byaddr.get(t); ann = f"  ; -> {n}" if n else (f"  ; +{t-va:x}" if va <= t < va+sz else "")
        elif ins.mnemonic.startswith(("cb", "tb", "b.")):
            t = ops[-1].imm; ann = f"  ; +{t-va:x}"
        print(f"  {ins.address-va:4x}: {ins.mnemonic:6s} {ins.op_str}{ann}")
