import sys, os, json, zipfile, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jcls import ClassFile, iter_code, INVOKE, FIELDOP, ACC_NATIVE

jar, IDX = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(jar)
CF = {}
def cls(n):
    if n not in CF:
        try:
            CF[n] = ClassFile(z.read(n + ".class"), want_code=True)
        except KeyError:
            CF[n] = None
    return CF[n]

def resolve(owner, name, desc):
    seen = set(); st = [owner]
    while st:
        c = st.pop()
        if c in seen or not c.startswith("org/lwjgl/"):
            continue
        seen.add(c); cf = cls(c)
        if cf is None:
            continue
        for m in cf.methods:
            if m["name"] == name and m["desc"] == desc:
                return c, m
        if cf.super: st.append(cf.super)
        st.extend(cf.interfaces)
    return None, None

refs = json.load(open(f"{IDX}/mc_lwjgl_refs.json"))["calls"]
roots = []
for k in refs:
    o, r = k.split(".", 1); i = r.index("(")
    roots.append((o, r[:i], r[i:]))

def is_glfn(s):
    return s.startswith("gl") and len(s) > 2 and s[2].isupper()

def walk(roots, stop_pkgs=()):
    seen = set(); natives = collections.Counter(); addr = collections.Counter()
    q = collections.deque(roots); via = collections.defaultdict(set)
    while q:
        o, n, d = q.popleft()
        key = (o, n, d)
        if key in seen: continue
        seen.add(key)
        c, m = resolve(o, n, d)
        if m is None: continue
        if m["acc"] & ACC_NATIVE:
            natives[f"{c}.{n}{d}"] += 1; continue
        code = m.get("code")
        if not code: continue
        cf = cls(c)
        for pc, op, a in iter_code(code):
            if op in INVOKE:
                t, to, tn, td = cf.ref(a)
                if to.startswith("org/lwjgl/") and not to.startswith(stop_pkgs):
                    q.append((to, tn, td))
            elif op == 0xb4 and c != "org/lwjgl/opengl/GLCapabilities":
                t, to, tn, td = cf.ref(a)
                if to == "org/lwjgl/opengl/GLCapabilities" and td == "J" and is_glfn(tn):
                    addr[tn] += 1; via[tn].add(f"{c}.{n}")
    return seen, natives, addr

seen, natives, addr = walk(roots, stop_pkgs=("org/lwjgl/glfw/",))
gl_nat = collections.Counter()
for k in natives:
    c, rest = k.split(".", 1); n = rest[:rest.index("(")]
    if c.startswith("org/lwjgl/opengl/"):
        cn = n[1:] if n.startswith("ngl") else n
        if is_glfn(cn): gl_nat[cn] += 1
other_nat = sorted(k for k in natives if not k.startswith("org/lwjgl/opengl/"))
need = sorted(set(gl_nat) | set(addr))

cap = cls("org/lwjgl/opengl/GLCapabilities")
checks = {}
for m in cap.methods:
    if m["name"].startswith("check_") and m.get("code"):
        ss = [cap.const(a) for pc, op, a in iter_code(m["code"]) if op in (0x12, 0x13)]
        checks[m["name"][6:]] = sorted({s for s in ss if isinstance(s, str) and is_glfn(s)})
json.dump(checks, open(f"{IDX}/glcaps_checks.json", "w"), indent=0)
json.dump({"gl_functions": need, "native_methods": sorted(natives), "addr_fields": sorted(addr)},
          open(f"{IDX}/gl_reach.json", "w"), indent=0)

print(f"[reach] visited_methods={len(seen)} natives={len(natives)} (opengl={sum(1 for k in natives if k.startswith('org/lwjgl/opengl/'))}, other={len(other_nat)})")
print(f"[reach] GL C-functions reachable: via native={len(gl_nat)} via GLCapabilities addr={len(addr)} union={len(need)}")
allck = set().union(*checks.values())
print(f"[caps] check sets={len(checks)} distinct fn names={len(allck)}; reachable∖caps={len(set(need)-allck)}")
for v in ["GL11", "GL12", "GL13", "GL14", "GL15", "GL20", "GL21", "GL30"]:
    s = set(checks.get(v, []))
    print(f"   {v}: requires={len(s):4d}  reachable_by_mc={len(s & set(need)):4d}")
