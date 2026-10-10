import sys, os, re, glob, json, subprocess

ROOT, WORK, MESA = (os.path.abspath(a) for a in sys.argv[1:4])
NDK = sys.argv[sys.argv.index("--ndk") + 1] if "--ndk" in sys.argv else None
T = os.path.dirname(os.path.abspath(__file__))
rc = 0

def sh(cmd, env=None, timeout=1800):
    r = subprocess.run(cmd, text=True, capture_output=True, env=env, timeout=timeout)
    return r.returncode, r.stdout + r.stderr

def build(tag, extra):
    b = os.path.join(WORK, f"build-{tag}")
    c, out = sh(["cmake", "-S", ROOT, "-B", b, "-G", "Ninja", *extra])
    if c == 0:
        c, out = sh(["cmake", "--build", b])
    warn = len(re.findall(r"\bwarning:", out))
    errs = [l for l in out.splitlines() if re.search(r"\berror\b|undefined reference", l)]
    print(f"[{tag}] build={'ok' if c == 0 else 'FAIL'} warnings={warn}")
    for l in (errs if c else [l for l in out.splitlines() if "warning:" in l])[:15]:
        print("   ", l[:220])
    return b if c == 0 else None

b = build("host", ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_COMPILER=clang++", "-DORYON_HOST_TEST=ON"])
if not b:
    sys.exit(1)
icd = glob.glob(os.path.join(MESA, "share/vulkan/icd.d/*.json"))
env = dict(os.environ, VK_DRIVER_FILES=icd[0] if icd else "", VK_ICD_FILENAMES=icd[0] if icd else "")
layer = os.path.exists("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
if layer:
    env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
c, out = sh([os.path.join(b, "oryon_smoke"), os.path.join(b, "liboryon.so")], env=env, timeout=600)
lines = out.splitlines()
verr = [l for l in lines if "Validation Error" in l or "VUID-" in l]
vwarn = [l for l in lines if "Validation Warning" in l or "Validation Performance Warning" in l]
print(f"[smoke] driver={os.path.basename(icd[0]) if icd else 'system'} validation_layer={'on' if layer else 'off'} "
      f"exit={c} validation_errors={len(verr)} validation_warnings={len(vwarn)}")
for l in lines:
    if l.startswith(("[smoke]", "[oryon]")) or "FAIL" in l:
        print("   ", l[:220])
for l in verr[:6]:
    print("    VAL:", l[:300])
rc |= c != 0 or bool(verr)

if NDK:
    a = build("arm64", ["-DCMAKE_BUILD_TYPE=Release", "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-26",
                        f"-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake"])
    if not a:
        sys.exit(1)
    so, js = os.path.join(a, "liboryon.so"), os.path.join(WORK, "oryon_elf.json")
    c, out = sh([sys.executable, "-I", os.path.join(T, "elfscan.py"), so, js]); print(out.strip())
    e = json.load(open(js))
    ex = sorted(x[0] for x in e["exports"])
    readme = open(os.path.join(ROOT, "README.md")).read()
    need = re.findall(r"`(egl[A-Z]\w+)`", next(l for l in readme.splitlines() if "fungsi diambil lewat" in l).split(":", 1)[1])
    extra = sorted(set(ex) - set(need)); miss = sorted(set(need) - set(ex))
    vkimp = [x[0] for x in e["imports"] if x[0].startswith("vk")]
    print(f"[arm64] size={os.path.getsize(so)} exports={len(ex)} launcher_egl_missing={len(miss)} extra_exports={len(extra)} "
          f"vk_imports={len(vkimp)} jni={sum(1 for x in ex if x.startswith(('Java_', 'JNI_')))}")
    if miss or extra:
        print("    missing:", miss, "extra:", extra[:10])
    rc |= bool(miss or extra or vkimp)
print(f"[hosttest] {'PASS' if not rc else 'FAIL'}")
sys.exit(rc)
