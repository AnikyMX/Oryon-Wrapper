import sys, os, re, json, collections

ROOT, IDX = sys.argv[1], sys.argv[2]
QUIET = "--quiet" in sys.argv
SO = sys.argv[sys.argv.index("--so") + 1] if "--so" in sys.argv else None
FILES = "--files" in sys.argv
L = lambda f: json.load(open(os.path.join(IDX, f)))
lw, caps, reach, pjx, vk = L("lwjgl.json"), L("glcaps_checks.json"), L("gl_reach.json"), L("pojavexec_elf.json"), L("vk_api.json")
ACC_NATIVE = 0x100
is_gl = lambda s: s.startswith("gl") and len(s) > 2 and s[2].isupper()

def kinds(desc):
    i, out = 1, []
    while desc[i] != ")":
        j = i
        while desc[j] == "[": j += 1
        if desc[j] == "L": j = desc.index(";", j)
        t = desc[i:j + 1]; out.append("J" if t[0] in "[L" else t); i = j + 1
    r = desc[i + 1:]
    return out, ("J" if r[0] in "[L" else r)

natives = collections.defaultdict(set)
for c, e in lw.items():
    if c.startswith("org/lwjgl/opengl/"):
        for n, d, a in e["m"]:
            if a & ACC_NATIVE:
                cn = n[1:] if n.startswith("ngl") else n
                if is_gl(cn):
                    natives[cn].add((c, n, d))
glconst = collections.defaultdict(set)
for c, e in lw.items():
    if c.startswith("org/lwjgl/opengl/"):
        for n, d, a, cv in e["f"]:
            if cv is not None and n.startswith("GL_"):
                glconst[n].add(cv)
capflags = {f[0] for f in lw["org/lwjgl/opengl/GLCapabilities"]["f"] if f[1] == "Z"}
mc_gl = set(reach["gl_functions"])
egl_req = sorted({s for _, s in pjx["strings"] if re.fullmatch(r"egl[A-Z]\w+", s)})

CTYPE = {"GLenum": "I", "GLint": "I", "GLuint": "I", "GLsizei": "I", "GLbitfield": "I", "GLhandleARB": "I",
         "GLfixed": "I", "GLfloat": "F", "GLclampf": "F", "GLdouble": "D", "GLclampd": "D", "GLboolean": "Z",
         "GLbyte": "B", "GLubyte": "B", "GLchar": "B", "GLshort": "S", "GLushort": "S", "GLhalf": "S",
         "GLsizeiptr": "J", "GLintptr": "J", "GLint64": "J", "GLuint64": "J", "GLsync": "J",
         "GLsizeiptrARB": "J", "GLintptrARB": "J", "GLDEBUGPROC": "J", "void": "V"}

def ptype(x):
    x = x.strip(); m = re.match(r"^(.*?[\s\*])(\w+)$", x)
    return m.group(1).strip() if m and m.group(1).strip() and m.group(2) not in CTYPE else x

def ckind(t):
    t = t.replace("const", "").strip()
    if "*" in t: return "J"
    return CTYPE.get(t.split()[-1] if t else t, "?")

VKOK = set(vk["commands"]) | set(vk["structs"]) | set(vk["enums"]) | set(vk["handles"]) | set(vk["typedefs"]) | \
    set(vk["funcptrs"]) | set(vk["enum_values"]) | set(vk["macros"])
VKOK |= {"VK_NO_PROTOTYPES", "VK_USE_PLATFORM_ANDROID_KHR", "VkFlags", "VkBool32", "VkDeviceSize", "VkSampleMask",
         "VK_DEFINE_HANDLE", "VK_DEFINE_NON_DISPATCHABLE_HANDLE", "VKAPI_PTR", "VKAPI_CALL", "VKAPI_ATTR"}
for fn in os.listdir(os.path.join(ROOT, "third_party/vulkan")):
    t = open(os.path.join(ROOT, "third_party/vulkan", fn), errors="ignore").read()
    VKOK |= set(re.findall(r"#\s*if(?:n?def)?\s+(?:defined\()?\s*(VK_\w+)", t))

SRC_EXT = (".cpp", ".hpp", ".h", ".inc", ".cc", ".c")
files = []
for dp, dn, fn in os.walk(ROOT):
    dn[:] = [d for d in dn if d not in ("third_party", "build", ".git", ".github")]
    files += [os.path.join(dp, f) for f in fn if f.endswith(SRC_EXT)]
files.sort()

def strip(t):
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group().count("\n"), t, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', t)

def args_at(s, i):
    d, cur, out, j = 0, "", [], i
    while j < len(s):
        ch = s[j]
        if ch in "([{": d += 1
        elif ch in ")]}":
            d -= 1
            if d == 0:
                if cur.strip(): out.append(cur.strip())
                return out, j
        if ch == "," and d == 1: out.append(cur.strip()); cur = ""
        elif d >= 1 and not (d == 1 and ch == "("): cur += ch
        j += 1
    return out, j

err, warn = [], []
gl_impl, egl_impl, procs, stubs, cap_ver, cap_ext = {}, {}, set(), set(), [], set()
jrefs = vkcalls = vkids = consts = desig = 0
per_file = {}
for p in files:
    rel = os.path.relpath(p, ROOT); raw = open(p, errors="ignore").read(); code = strip(raw)
    snap = (len(err), vkids, vkcalls, desig, consts, len(gl_impl), len(egl_impl))
    for m in re.finditer(r"org/lwjgl/[\w/$]+(?:\.([\w<>]+)(\([^)\s]*\)[\w/;\[$]+))?", raw):
        jrefs += 1
        cls = m.group(0).split(".")[0]
        if cls not in lw: err.append(f"{rel}: java class not in lwjgl jar: {cls}"); continue
        if m.group(1) and not any(x[0] == m.group(1) and x[1] == m.group(2) for x in lw[cls]["m"]):
            err.append(f"{rel}: java method not found: {m.group(0)}")
    for m in re.finditer(r"\bOGL_API\s+([\w\s\*]+?)\s*\b(gl[A-Z]\w*)\s*\(", code):
        ret, name = m.group(1), m.group(2); a, _ = args_at(code, m.end() - 1)
        a = [x for x in a if x and x != "void"]
        gl_impl.setdefault(name, (rel, ret, a))
    for m in re.finditer(r"\bOEGL_API\s+([\w\s\*]+?)\s*\b(egl[A-Z]\w*)\s*\(", code):
        egl_impl.setdefault(m.group(2), rel)
    procs |= set(re.findall(r"\bORYON_PROC\(\s*(gl\w+)\s*\)", code))
    stubs |= set(re.findall(r"\bORYON_STUB\(\s*(gl\w+)\s*\)", code))
    cap_ver += [(int(a), int(b)) for a, b in re.findall(r"\bORYON_CAP_VERSION\(\s*(\d)\s*,\s*(\d)\s*\)", code)]
    cap_ext |= set(re.findall(r"\bORYON_CAP_EXT\(\s*(GL_\w+)\s*\)", code))
    for m in re.finditer(r"\bconstexpr\s+\w+\s+(GL_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?\s*;", code):
        consts += 1; n, v = m.group(1), int(m.group(2), 0)
        if n not in glconst: err.append(f"{rel}: GL constant not in lwjgl: {n}")
        elif v not in glconst[n]: err.append(f"{rel}: GL constant value mismatch: {n}={v:#x}")
    for m in re.finditer(r"\b(?:vk|Vk|VK_|PFN_vk)\w*", code):
        w = m.group(0); vkids += 1
        if w.startswith("PFN_"):
            if w not in VKOK: err.append(f"{rel}: unknown {w}")
        elif w not in VKOK and not w.startswith(("vkd", "vki", "vkg")):
            err.append(f"{rel}: unknown vulkan symbol {w}")
    for m in re.finditer(r"(?<![\w])(vk[A-Z]\w*)\s*\(", code):
        n = m.group(1)
        if n in vk["commands"]:
            a, _ = args_at(code, m.end() - 1); vkcalls += 1
            if len(a) != len(vk["commands"][n]["params"]):
                err.append(f"{rel}: {n} args {len(a)} != {len(vk['commands'][n]['params'])}")
    for m in re.finditer(r"\b(Vk[A-Z]\w*)\s+\w+\s*(?:=\s*)?\{|\b(Vk[A-Z]\w*)\s*\{", code):
        st = m.group(1) or m.group(2)
        if st not in vk["structs"]: continue
        blk, depth, j = "", 0, m.end() - 1
        while j < len(code):
            ch = code[j]; depth += ch == "{"; depth -= ch == "}"
            blk += ch; j += 1
            if depth == 0: break
        top, d = [], 0
        for k, ch in enumerate(blk):
            if ch in "{([": d += 1
            elif ch in "})]": d -= 1
            elif ch == "." and d == 1:
                mm = re.match(r"\.(\w+)\s*=", blk[k:])
                if mm: top.append(mm.group(1))
        mem = [x[1] for x in vk["structs"][st]["members"]]
        for t in top:
            desig += 1
            if t not in mem: err.append(f"{rel}: {st} has no member .{t}")
        idx = [mem.index(t) for t in top if t in mem]
        if idx != sorted(idx): err.append(f"{rel}: {st} designated initializers out of order")
    per_file[rel] = [len(err) - snap[0], vkids - snap[1], vkcalls - snap[2], desig - snap[3], consts - snap[4],
                     len(gl_impl) - snap[5], len(egl_impl) - snap[6]]

for n, (rel, ret, a) in sorted(gl_impl.items()):
    if n not in natives:
        err.append(f"{rel}: {n} has no LWJGL native"); continue
    ck = [ckind(ptype(x)) for x in a]
    ok = False
    for c, jn, d in natives[n]:
        ja, jr = kinds(d)
        if ja == ck and (jr == ckind(ret) or (jr == "J" and "*" in ret)):
            ok = True; break
    if not ok:
        c, jn, d = sorted(natives[n])[0]
        err.append(f"{rel}: {n}({','.join(ck)})->{ckind(ret)} != {c.rsplit('/',1)[1]}.{jn}{d}")
table = procs | stubs
for n in procs:
    if n not in gl_impl: err.append(f"proc table: {n} marked PROC but no OGL_API definition")
for n in stubs:
    if n in mc_gl: err.append(f"proc table: {n} is used by MC 1.12.2 but only stubbed")
    if n not in natives: err.append(f"proc table: stub {n} unknown to LWJGL")
claimed = set()
for M, m in cap_ver:
    for v in range(10, M * 10 + m + 1):
        f = f"OpenGL{v}"
        if f in capflags: claimed.add(f)
claimed |= cap_ext
for f in sorted(claimed):
    if f not in capflags: err.append(f"caps: {f} is not a GLCapabilities flag"); continue
    key = f.replace("OpenGL", "GL") if f.startswith("OpenGL") else f[3:]
    miss = set(caps.get(key, [])) - table
    if miss: err.append(f"caps: {f} requires {len(miss)} more entry points")
for n in egl_req:
    if table or egl_impl:
        if n not in egl_impl: err.append(f"egl: launcher requires {n} (missing)")
for n in egl_impl:
    if n not in egl_req: warn.append(f"egl: {n} not used by libpojavexec")

exports = None
if SO:
    from elftools.elf.elffile import ELFFile
    with open(SO, "rb") as f:
        exports = {y.name for y in ELFFile(f).get_section_by_name(".dynsym").iter_symbols()
                   if y.name and y["st_shndx"] != "SHN_UNDEF" and y["st_info"]["bind"] in ("STB_GLOBAL", "STB_WEAK")
                   and y["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT")}
    want = set(gl_impl) | set(egl_impl)
    err += [f"so: {n} declared but not exported" for n in sorted(want - exports)]
    err += [f"so: unexpected export {n}" for n in sorted(exports - want)]

impl_mc = len(mc_gl & set(gl_impl))
print(f"[validate] files={len(files)} java_refs={jrefs} gl_impl={len(gl_impl)} procs={len(procs)} stubs={len(stubs)} "
      f"egl={len(egl_impl)}/{len(egl_req)} claimed_flags={len(claimed)} gl_consts={consts}")
print(f"[validate] vk_ids={vkids} vk_calls={vkcalls} vk_designators={desig}  mc_gl_coverage={impl_mc}/{len(mc_gl)}")
if exports is not None:
    print(f"[validate] so={os.path.basename(SO)} exports={len(exports)}")
if FILES:
    for rel, (e, vi, vc, de, co, g, eg) in per_file.items():
        tag = " ".join(f"{k}={v}" for k, v in (("gl", g), ("egl", eg), ("const", co), ("vk_id", vi), ("vk_call", vc),
                                                ("desig", de)) if v)
        print(f"   {rel:24s} {tag or '-'}  {'OK' if not e else f'ERR={e}'}")
print(f"[validate] errors={len(err)} warnings={len(warn)} -> {'PASS' if not err else 'FAIL'}")
for x in (err if QUIET else err + warn)[:60]:
    print("   -", x)
sys.exit(1 if err else 0)
