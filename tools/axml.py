import sys, struct, zipfile

def strpool(b, off):
    t, hs, sz, cnt, scnt, flags, sstart, _ = struct.unpack_from("<HHIIIIII", b, off)
    utf8 = bool(flags & 0x100); offs = struct.unpack_from(f"<{cnt}I", b, off + hs)
    out = []
    for o in offs:
        p = off + sstart + o
        if utf8:
            n = b[p]; p += 2 if n & 0x80 else 1
            n = b[p]
            if n & 0x80: n = ((n & 0x7f) << 8) | b[p + 1]; p += 2
            else: p += 1
            out.append(b[p:p + n].decode("utf-8", "replace"))
        else:
            n = struct.unpack_from("<H", b, p)[0]; p += 2
            if n & 0x8000: n = ((n & 0x7fff) << 16) | struct.unpack_from("<H", b, p)[0]; p += 2
            out.append(b[p:p + 2 * n].decode("utf-16le", "replace"))
    return out, sz

def arsc(b):
    res = {}; hs = struct.unpack_from("<H", b, 2)[0]; off = hs; gstr = None
    while off < len(b):
        t, h, sz = struct.unpack_from("<HHI", b, off)
        if t == 0x0001: gstr, _ = strpool(b, off)
        elif t == 0x0200:
            pid = struct.unpack_from("<I", b, off + 8)[0]
            ts_off, _, ks_off = struct.unpack_from("<III", b, off + 268)
            tstr, _ = strpool(b, off + ts_off); kstr, _ = strpool(b, off + ks_off)
            q = off + h
            while q < off + sz:
                ct, chs, csz = struct.unpack_from("<HHI", b, q)
                if ct == 0x0201:
                    tid = b[q + 8]; ecount, estart = struct.unpack_from("<II", b, q + 12)
                    eo = struct.unpack_from(f"<{ecount}I", b, q + chs)
                    for i, e in enumerate(eo):
                        if e == 0xFFFFFFFF: continue
                        p = q + estart + e; esz, ef, key = struct.unpack_from("<HHI", b, p)
                        rid = (pid << 24) | (tid << 16) | i
                        if ef & 1: val = "<complex>"
                        else:
                            _, _, dt, data = struct.unpack_from("<HBBI", b, p + esz)
                            val = gstr[data] if dt == 3 else (bool(data) if dt == 0x12 else data)
                        res.setdefault(rid, (tstr[tid - 1], kstr[key], val))
                q += csz
        off += sz
    return res

def axml(b, res=None):
    strs = []; rmap = []; off = 8; depth = 0; out = []
    while off < len(b):
        t, h, sz = struct.unpack_from("<HHI", b, off)
        if t == 0x0001: strs, _ = strpool(b, off)
        elif t == 0x0180: rmap = list(struct.unpack_from(f"<{(sz - h) // 4}I", b, off + h))
        elif t == 0x0102:
            ns, name, ast, asz, acnt = struct.unpack_from("<IIHHH", b, off + 16)
            attrs = []
            for i in range(acnt):
                a = off + 16 + ast + i * asz
                ans, an, raw, _, _, dt, data = struct.unpack_from("<IIIHBBI", b, a)
                nm = strs[an] if an < len(strs) and strs[an] else f"0x{rmap[an]:08x}"
                if dt == 3: v = repr(strs[data])
                elif dt == 0x12: v = "true" if data else "false"
                elif dt == 0x01:
                    r = (res or {}).get(data)
                    v = f"@{r[0]}/{r[1]}={r[2]!r}" if r else f"@0x{data:08x}"
                elif dt in (0x10, 0x11): v = str(data if data < 0x80000000 else data - (1 << 32))
                else: v = f"(t{dt:#x}){data:#x}"
                attrs.append(f"{nm}={v}")
            out.append("  " * depth + f"<{strs[name]} " + " ".join(attrs) + ">"); depth += 1
        elif t == 0x0103: depth -= 1
        off += sz
    return out

if __name__ == "__main__":
    z = zipfile.ZipFile(sys.argv[1]); ent = sys.argv[2] if len(sys.argv) > 2 else "AndroidManifest.xml"
    res = arsc(z.read("resources.arsc")) if "resources.arsc" in z.namelist() else {}
    for line in axml(z.read(ent), res):
        print(line)
    if "--res" in sys.argv:
        for k, v in sorted(res.items()):
            print(f"res 0x{k:08x} {v[0]}/{v[1]} = {v[2]!r}")
