import sys, os, subprocess, hashlib, shutil

UP, PRJ, IDX = (os.path.abspath(a) for a in sys.argv[1:4])
T = os.path.dirname(os.path.abspath(__file__))
os.makedirs(IDX, exist_ok=True)

def find(suffix, need=True):
    for n in sorted(os.listdir(UP)):
        if n.endswith(suffix):
            return os.path.join(UP, n)
    if need:
        sys.exit(f"missing input *{suffix} in {UP}")

jar_lw, jar_mc, so = find("lwjgl-glfw-classes.jar"), find("1.12.2.jar"), find("libpojavexec.so", False)
for p in (jar_lw, jar_mc, so) if so else (jar_lw, jar_mc):
    h = hashlib.sha256(open(p, "rb").read()).hexdigest()[:16]
    print(f"[input] {os.path.basename(p)} {os.path.getsize(p)} B sha256:{h}")

def run(*a):
    r = subprocess.run([sys.executable, "-I", *a], text=True, capture_output=True)
    sys.stdout.write(r.stdout); sys.stderr.write(r.stderr[-2000:])
    return r.returncode

steps = [
    (os.path.join(T, "jarindex.py"), jar_lw, jar_mc, IDX),
    (os.path.join(T, "reach.py"), jar_lw, IDX),
    (os.path.join(T, "vkindex.py"), os.path.join(PRJ, "third_party/vulkan"), os.path.join(IDX, "vk_api.json")),
]
if so:
    steps.append((os.path.join(T, "elfscan.py"), so, os.path.join(IDX, "pojavexec_elf.json")))
else:
    import re, json
    line = next(l for l in open(os.path.join(PRJ, "README.md")) if "fungsi diambil lewat" in l)
    egl = re.findall(r"`(egl[A-Z]\w+)`", line.split(":", 1)[1])
    json.dump({"strings": [[0, s] for s in egl]}, open(os.path.join(IDX, "pojavexec_elf.json"), "w"))
    print(f"[input] libpojavexec.so absent -> README egl list ({len(egl)})")
for s in steps:
    if run(*s):
        sys.exit(f"step failed: {os.path.basename(s[0])}")
print("[env] " + " | ".join(
    f"{t}={'yes' if shutil.which(t) else 'no'}" for t in ("vulkaninfo", "glslangValidator", "spirv-val", "cmake", "clang++")))
rc = run(os.path.join(T, "validate.py"), PRJ, IDX)
sys.exit(rc)
