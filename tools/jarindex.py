import sys, os, json, zipfile, collections, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jcls import ClassFile, iter_code, arg_types, INVOKE, FIELDOP, T_INT, T_FLOAT, ACC_NATIVE, ACC_STATIC

LJ, MJ, OUT = sys.argv[1:4]
os.makedirs(OUT, exist_ok=True)

lw = {}
lw_other = collections.Counter()
with zipfile.ZipFile(LJ) as z:
    names = z.namelist()
    for n in names:
        if not n.endswith(".class"):
            lw_other[os.path.splitext(n)[1] or "<dir>"] += 1
            continue
        if n.startswith("META-INF/versions/"):
            continue
        cf = ClassFile(z.read(n))
        lw[cf.name] = {
            "super": cf.super, "itf": cf.interfaces, "acc": cf.access, "major": cf.major,
            "f": [[m["name"], m["desc"], m["acc"], m.get("cv")] for m in cf.fields],
            "m": [[m["name"], m["desc"], m["acc"]] for m in cf.methods],
            "s": sorted(set(cf.strings())),
        }
json.dump(lw, open(f"{OUT}/lwjgl.json", "w"))

pk = collections.Counter(c.rsplit("/", 1)[0] for c in lw)
nat = sum(1 for c in lw.values() for m in c["m"] if m[2] & ACC_NATIVE)
print(f"[lwjgl] classes={len(lw)} packages={len(pk)} native_methods={nat} nonclass_entries={sum(lw_other.values())}")
for p, c in sorted(pk.items(), key=lambda x: -x[1])[:12]:
    print(f"   {p:34s} {c}")

CONSTOP = {0x02: -1, 0x03: 0, 0x04: 1, 0x05: 2, 0x06: 3, 0x07: 4, 0x08: 5,
           0x0b: 0.0, 0x0c: 1.0, 0x0d: 2.0, 0x09: 0, 0x0a: 1, 0x0e: 0.0, 0x0f: 1.0}
SIMPLE = set(range(0x15, 0x2e)) | {0x01, 0xb2}
calls = {}
frefs = {}
mc_classes = 0; mc_res = collections.Counter(); mc_methods = 0
with zipfile.ZipFile(MJ) as z:
    for n in z.namelist():
        if not n.endswith(".class"):
            top = n.split("/")[0] if "/" in n else "<root>"
            mc_res[top] += 1
            continue
        cf = ClassFile(z.read(n), want_code=True)
        mc_classes += 1
        for m in cf.methods:
            code = m.get("code")
            if not code:
                continue
            mc_methods += 1
            win = []
            for pc, op, arg in iter_code(code):
                if op in INVOKE:
                    t, owner, nm, ds = cf.ref(arg)
                    if owner.startswith("org/lwjgl/"):
                        k = f"{owner}.{nm}{ds}"
                        e = calls.setdefault(k, {"n": 0, "k": set(), "cl": set(), "c": collections.Counter()})
                        e["n"] += 1; e["k"].add(INVOKE[op]); e["cl"].add(cf.name)
                        at, _ = arg_types(ds)
                        na = len(at) + (0 if op == 0xb8 else 1)
                        tail = win[-na:] if na else []
                        if na and len(tail) == na and all(x[0] in ("c", "s") for x in tail):
                            vals = tuple(x[1] if x[0] == "c" else None for x in tail[-len(at):]) if at else ()
                            if any(v is not None for v in vals):
                                e["c"][repr(vals)] += 1
                    win.append(("x", None))
                elif op in FIELDOP:
                    t, owner, nm, ds = cf.ref(arg)
                    if owner.startswith("org/lwjgl/"):
                        k = f"{owner}.{nm}:{ds}"
                        e = frefs.setdefault(k, {"n": 0, "k": set(), "cl": set()})
                        e["n"] += 1; e["k"].add(FIELDOP[op]); e["cl"].add(cf.name)
                    win.append(("s", None) if op == 0xb2 else ("x", None))
                elif op in CONSTOP:
                    win.append(("c", CONSTOP[op]))
                elif op in (0x10, 0x11):
                    win.append(("c", arg))
                elif op in (0x12, 0x13):
                    v = cf.const(arg)
                    win.append(("c", v) if isinstance(v, (int, float)) else ("s", None))
                elif op in SIMPLE:
                    win.append(("s", None))
                else:
                    win.append(("x", None))
                if len(win) > 16:
                    del win[:8]

def ser(d):
    o = {}
    for k, e in d.items():
        r = {"n": e["n"], "k": sorted(e["k"]), "callers": len(e["cl"])}
        if "c" in e and e["c"]:
            r["consts"] = dict(e["c"].most_common())
        o[k] = r
    return o

json.dump({"calls": ser(calls), "fields": ser(frefs)}, open(f"{OUT}/mc_lwjgl_refs.json", "w"))
print(f"[mc] classes={mc_classes} methods_with_code={mc_methods} resources={sum(mc_res.values())}")
print(f"[mc] distinct lwjgl method refs={len(calls)} call_sites={sum(e['n'] for e in calls.values())} "
      f"distinct field refs={len(frefs)}")
byown = collections.Counter(k.split(".")[0].rsplit("/", 1)[0] for k in calls)
for p, c in byown.most_common():
    print(f"   {p:34s} methods={c}")

def resolve(owner, nm, ds, field=False):
    seen = set(); stack = [owner]
    while stack:
        c = stack.pop()
        if c in seen:
            continue
        seen.add(c)
        e = lw.get(c)
        if e is None:
            if not c.startswith("org/lwjgl/"):
                return "external"
            continue
        for m in (e["f"] if field else e["m"]):
            if m[0] == nm and m[1] == ds:
                return "ok"
        if e["super"]:
            stack.append(e["super"])
        stack.extend(e["itf"])
    return "missing_member" if owner in lw else "missing_class"

xr = {"calls": {}, "fields": {}}
tally = collections.Counter(); tally_pk = collections.defaultdict(collections.Counter)
for k in calls:
    owner, rest = k.split(".", 1)
    nm = rest[:rest.index("(")]; ds = rest[rest.index("("):]
    r = resolve(owner, nm, ds)
    xr["calls"][k] = r; tally["m_" + r] += 1; tally_pk[owner.rsplit("/", 1)[0]][r] += 1
for k in frefs:
    owner, rest = k.split(".", 1)
    nm, ds = rest.split(":", 1)
    r = resolve(owner, nm, ds, field=True)
    xr["fields"][k] = r; tally["f_" + r] += 1; tally_pk[owner.rsplit("/", 1)[0]]["f_" + r] += 1
json.dump(xr, open(f"{OUT}/xref_mc_vs_lwjgl.json", "w"), indent=0)
print("[xref] " + " ".join(f"{k}={v}" for k, v in sorted(tally.items())))
for p, c in sorted(tally_pk.items()):
    print(f"   {p:34s} " + " ".join(f"{k}={v}" for k, v in sorted(c.items())))
