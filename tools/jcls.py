import struct

T_UTF8, T_INT, T_FLOAT, T_LONG, T_DOUBLE, T_CLASS, T_STR = 1, 3, 4, 5, 6, 7, 8
T_FREF, T_MREF, T_IMREF, T_NAT, T_MH, T_MT, T_DYN, T_INDY, T_MOD, T_PKG = 9, 10, 11, 12, 15, 16, 17, 18, 19, 20

ACC_STATIC, ACC_NATIVE, ACC_PUBLIC = 0x0008, 0x0100, 0x0001

OPLEN = [1] * 256
for o in (0x10, 0x12, 0x15, 0x16, 0x17, 0x18, 0x19, 0x36, 0x37, 0x38, 0x39, 0x3a, 0xa9, 0xbc):
    OPLEN[o] = 2
for o in (0x11, 0x13, 0x14, 0x84, 0xa7, 0xa8, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
          0xbb, 0xbd, 0xc0, 0xc1, 0xc6, 0xc7) + tuple(range(0x99, 0xa7)):
    OPLEN[o] = 3
for o in (0xb9, 0xba, 0xc8, 0xc9):
    OPLEN[o] = 5
OPLEN[0xc5] = 4
OPLEN[0xaa] = OPLEN[0xab] = OPLEN[0xc4] = 0

INVOKE = {0xb6: "virtual", 0xb7: "special", 0xb8: "static", 0xb9: "interface"}
FIELDOP = {0xb2: "getstatic", 0xb3: "putstatic", 0xb4: "getfield", 0xb5: "putfield"}


class ClassFile:
    __slots__ = ("cp", "access", "name", "super", "interfaces", "fields", "methods", "major")

    def __init__(self, data, want_code=False):
        if data[:4] != b"\xca\xfe\xba\xbe":
            raise ValueError("bad magic")
        self.major = struct.unpack_from(">H", data, 6)[0]
        n = struct.unpack_from(">H", data, 8)[0]
        cp = [None] * n
        p = 10; i = 1
        while i < n:
            t = data[p]
            if t == T_UTF8:
                ln = struct.unpack_from(">H", data, p + 1)[0]
                cp[i] = (t, data[p + 3:p + 3 + ln].decode("utf-8", "replace")); p += 3 + ln
            elif t in (T_INT, T_FLOAT):
                fmt = ">i" if t == T_INT else ">f"
                cp[i] = (t, struct.unpack_from(fmt, data, p + 1)[0]); p += 5
            elif t in (T_LONG, T_DOUBLE):
                fmt = ">q" if t == T_LONG else ">d"
                cp[i] = (t, struct.unpack_from(fmt, data, p + 1)[0]); p += 9; i += 1
            elif t in (T_CLASS, T_STR, T_MT, T_MOD, T_PKG):
                cp[i] = (t, struct.unpack_from(">H", data, p + 1)[0]); p += 3
            elif t in (T_FREF, T_MREF, T_IMREF, T_NAT, T_DYN, T_INDY):
                cp[i] = (t,) + struct.unpack_from(">HH", data, p + 1); p += 5
            elif t == T_MH:
                cp[i] = (t, data[p + 1], struct.unpack_from(">H", data, p + 2)[0]); p += 4
            else:
                raise ValueError(f"cp tag {t}")
            i += 1
        self.cp = cp
        self.access, ti, si = struct.unpack_from(">HHH", data, p); p += 6
        self.name = self.cls(ti)
        self.super = self.cls(si) if si else None
        ic = struct.unpack_from(">H", data, p)[0]; p += 2
        self.interfaces = [self.cls(struct.unpack_from(">H", data, p + 2 * k)[0]) for k in range(ic)]
        p += 2 * ic
        self.fields, p = self._members(data, p, False)
        self.methods, p = self._members(data, p, want_code)

    def utf(self, i):
        return self.cp[i][1]

    def cls(self, i):
        return self.utf(self.cp[i][1])

    def const(self, i):
        e = self.cp[i]
        if e[0] in (T_INT, T_FLOAT, T_LONG, T_DOUBLE):
            return e[1]
        if e[0] == T_STR:
            return self.utf(e[1])
        return None

    def ref(self, i):
        t, ci, ni = self.cp[i]
        _, nn, dd = self.cp[ni]
        return t, self.cls(ci), self.utf(nn), self.utf(dd)

    def _members(self, data, p, want_code):
        cnt = struct.unpack_from(">H", data, p)[0]; p += 2
        out = []
        for _ in range(cnt):
            acc, ni, di, ac = struct.unpack_from(">HHHH", data, p); p += 8
            m = {"name": self.utf(ni), "desc": self.utf(di), "acc": acc}
            for _ in range(ac):
                an, al = struct.unpack_from(">HI", data, p); p += 6
                aname = self.utf(an)
                if aname == "ConstantValue":
                    m["cv"] = self.const(struct.unpack_from(">H", data, p)[0])
                elif aname == "Code" and want_code:
                    clen = struct.unpack_from(">I", data, p + 4)[0]
                    m["code"] = data[p + 8:p + 8 + clen]
                p += al
            out.append(m)
        return out, p

    def strings(self):
        return [self.utf(e[1]) for e in self.cp if e and e[0] == T_STR]


def iter_code(code):
    pc = 0; n = len(code)
    while pc < n:
        op = code[pc]; ln = OPLEN[op]
        if ln == 0:
            if op == 0xc4:
                ln = 6 if code[pc + 1] == 0x84 else 4
            else:
                pad = (4 - ((pc + 1) % 4)) % 4
                q = pc + 1 + pad
                if op == 0xaa:
                    lo, hi = struct.unpack_from(">ii", code, q + 4)
                    ln = 1 + pad + 12 + 4 * (hi - lo + 1)
                else:
                    np_ = struct.unpack_from(">i", code, q + 4)[0]
                    ln = 1 + pad + 8 + 8 * np_
            yield pc, op, None
        elif op in (0x10,):
            yield pc, op, struct.unpack_from(">b", code, pc + 1)[0]
        elif op == 0x11:
            yield pc, op, struct.unpack_from(">h", code, pc + 1)[0]
        elif op == 0x12:
            yield pc, op, code[pc + 1]
        elif ln >= 3 and op in (0x13, 0x14, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbd, 0xc0, 0xc1):
            yield pc, op, struct.unpack_from(">H", code, pc + 1)[0]
        else:
            yield pc, op, None
        pc += ln


def arg_types(desc):
    i = 1; out = []
    while desc[i] != ")":
        j = i
        while desc[j] == "[":
            j += 1
        if desc[j] == "L":
            j = desc.index(";", j)
        out.append(desc[i:j + 1]); i = j + 1
    return out, desc[i + 1:]
