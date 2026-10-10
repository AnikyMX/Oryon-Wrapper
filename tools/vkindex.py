import sys, os, re, json

vdir, out = sys.argv[1], sys.argv[2]
api = {"commands": {}, "structs": {}, "enums": {}, "enum_values": {}, "macros": {}, "extensions": {},
       "handles": [], "typedefs": {}, "funcptrs": [], "files": {}}
for fn in sorted(os.listdir(vdir)):
    if not fn.endswith(".h"):
        continue
    txt = open(os.path.join(vdir, fn), errors="ignore").read()
    api["files"][fn] = len(txt)
    body = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    for m in re.finditer(r"VKAPI_ATTR\s+(.+?)\s+VKAPI_CALL\s+(vk\w+)\s*\((.*?)\);", body, re.S):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
        ps = []
        for p in [x.strip() for x in params.split(",") if x.strip()]:
            p = " ".join(p.split())
            if p == "void":
                continue
            mm = re.match(r"(.*?)(\w+)(\s*\[[^\]]*\])?$", p)
            ps.append([mm.group(1).strip(), mm.group(2)])
        api["commands"][name] = {"ret": " ".join(ret.split()), "params": ps, "file": fn}
    for m in re.finditer(r"typedef\s+(struct|union)\s+(Vk\w+)\s*\{(.*?)\}\s*\w+\s*;", body, re.S):
        mem = []
        for line in m.group(3).split(";"):
            line = " ".join(line.split())
            if not line:
                continue
            mm = re.match(r"(.*?)(\w+)(\s*\[[^\]]*\])?$", line)
            if mm:
                mem.append([mm.group(1).strip(), mm.group(2)])
        api["structs"][m.group(2)] = {"kind": m.group(1), "members": mem, "file": fn}
    for m in re.finditer(r"typedef\s+enum\s+(Vk\w+)\s*\{(.*?)\}\s*\w+\s*;", body, re.S):
        vals = {}
        for line in m.group(2).split(","):
            mm = re.match(r"\s*(VK_\w+)\s*=\s*(.+?)\s*$", line.strip())
            if mm:
                vals[mm.group(1)] = mm.group(2)
        api["enums"][m.group(1)] = vals
        api["enum_values"].update({k: m.group(1) for k in vals})
    for m in re.finditer(r"^\s*#define\s+(VK_\w+)(?:\(|\s+|$)(.*)$", body, re.M):
        api["macros"][m.group(1)] = (m.group(2) or "").strip()
    for m in re.finditer(r"VK_DEFINE_(?:NON_DISPATCHABLE_)?HANDLE\((Vk\w+)\)", body):
        api["handles"].append(m.group(1))
    for m in re.finditer(r"typedef\s+(\w+)\s+(Vk\w+)\s*;", body):
        api["typedefs"][m.group(2)] = m.group(1)
    for m in re.finditer(r"typedef\s+.*?\(VKAPI_PTR\s*\*\s*(PFN_\w+)\)", body, re.S):
        api["funcptrs"].append(m.group(1))
for k, v in api["macros"].items():
    if k.endswith("_EXTENSION_NAME"):
        base = k[:-len("_EXTENSION_NAME")]
        api["extensions"][v.strip('"')] = {"macro": base, "spec": api["macros"].get(base + "_SPEC_VERSION")}
json.dump(api, open(out, "w"), indent=0)
print(f"[vk] files={list(api['files'])} header_version={api['macros'].get('VK_HEADER_VERSION')}")
print(f"[vk] commands={len(api['commands'])} structs={len(api['structs'])} enums={len(api['enums'])} "
      f"enum_values={len(api['enum_values'])} handles={len(api['handles'])} extensions={len(api['extensions'])}")
