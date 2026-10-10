import sys, os, zipfile, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jcls import ClassFile, iter_code, INVOKE, FIELDOP

BR = {**{o: "if" for o in range(0x99, 0xa7)}, 0xc6: "ifnull", 0xc7: "ifnonnull", 0xa7: "goto"}
jar, cls = sys.argv[1], sys.argv[2]
INTS = "--ints" in sys.argv
want = set(a for a in sys.argv[3:] if a != "--ints")
with zipfile.ZipFile(jar) as z:
    cf = ClassFile(z.read(cls + ".class"), want_code=True)
for m in cf.methods:
    if want and m["name"] not in want:
        continue
    code = m.get("code")
    print(f"## {m['name']}{m['desc']} acc=0x{m['acc']:x} len={len(code) if code else 0}")
    if not code:
        continue
    out = []
    for pc, op, arg in iter_code(code):
        if op in INVOKE:
            t, o, n, d = cf.ref(arg)
            out.append(f"{pc}:call {o.rsplit('/',1)[-1]}.{n}")
        elif op in FIELDOP:
            t, o, n, d = cf.ref(arg)
            out.append(f"{pc}:{FIELDOP[op][:4]} {o.rsplit('/',1)[-1]}.{n}")
        elif op in (0x12, 0x13):
            v = cf.const(arg)
            if isinstance(v, str):
                out.append(f"{pc}:str '{v}'")
            elif INTS and isinstance(v, int):
                out.append(f"{pc}:#{v:#x}")
        elif INTS and op in (0x10, 0x11):
            out.append(f"{pc}:#{arg:#x}")
        elif INTS and 0x02 <= op <= 0x08:
            out.append(f"{pc}:#{op-3}")
        elif op in BR:
            off = struct.unpack_from(">h", code, pc + 1)[0]
            out.append(f"{pc}:{BR[op]}->{pc+off}")
        elif op in (0xaa, 0xab):
            out.append(f"{pc}:switch")
        elif 0xac <= op <= 0xb1:
            out.append(f"{pc}:ret")
        elif op == 0xbf:
            out.append(f"{pc}:throw")
    print("  " + " | ".join(out))
