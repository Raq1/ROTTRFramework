"""DXBC container, SM4/5 token encoding and the DXBC checksum (a modified MD5)."""
import struct, math, ctypes

# ---- MD5 compression (the DXBC checksum pads its own way) ----
_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]

def _rol(x, c):
    x &= 0xFFFFFFFF
    return ((x << c) | (x >> (32 - c))) & 0xFFFFFFFF

def _compress(state, block):
    a, b, c, d = state
    m = struct.unpack('<16I', block)
    for i in range(64):
        if i < 16:
            f = (b & c) | (~b & d); g = i
        elif i < 32:
            f = (d & b) | (~d & c); g = (5 * i + 1) % 16
        elif i < 48:
            f = b ^ c ^ d; g = (3 * i + 5) % 16
        else:
            f = c ^ (b | (~d & 0xFFFFFFFF)); g = (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xFFFFFFFF
        a, d, c, b = d, c, b, (b + _rol(f, _S[i])) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]

def checksum(blob):
    d = blob[20:]
    size = len(d)
    bits = (size * 8) & 0xFFFFFFFF
    bits2 = ((bits >> 2) | 1) & 0xFFFFFFFF
    state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    full = size // 64 * 64
    for off in range(0, full, 64):
        state = _compress(state, d[off:off + 64])
    left = d[full:]
    if len(left) >= 56:
        state = _compress(state, left + b'\x80' + b'\0' * (63 - len(left)))
        state = _compress(state, struct.pack('<I', bits) + b'\0' * 56 + struct.pack('<I', bits2))
    else:
        block = struct.pack('<I', bits) + left + b'\x80'
        block += b'\0' * (60 - len(block)) + struct.pack('<I', bits2)
        state = _compress(state, block)
    return struct.pack('<4I', *state)

# ---- container ----
def chunks(blob):
    count = struct.unpack_from('<I', blob, 28)[0]
    out = []
    for i in range(count):
        off = struct.unpack_from('<I', blob, 32 + 4 * i)[0]
        tag = blob[off:off + 4]
        size = struct.unpack_from('<I', blob, off + 4)[0]
        out.append((tag, blob[off + 8:off + 8 + size]))
    return out

def build(chunk_list):
    count = len(chunk_list)
    header = 32 + 4 * count
    offs, body = [], b''
    for tag, data in chunk_list:
        offs.append(header + len(body))
        body += tag + struct.pack('<I', len(data)) + data
    total = header + len(body)
    blob = b'DXBC' + b'\0' * 16 + struct.pack('<III', 1, total, count) + struct.pack('<%dI' % count, *offs) + body
    return blob[:4] + checksum(blob) + blob[20:]

# ---- shader code ----
CUSTOMDATA = 0x35

def instructions(code):
    """code: SHEX/SHDR chunk data -> (version token, [token lists])"""
    toks = list(struct.unpack('<%dI' % (len(code) // 4), code))
    version, length = toks[0], toks[1]
    i, out = 2, []
    while i < length:
        t = toks[i]
        n = toks[i + 1] if (t & 0x7FF) == CUSTOMDATA else (t >> 24) & 0x7F
        assert n > 0, ('zero length', i)
        out.append(toks[i:i + n])
        i += n
    return version, out

def code_bytes(version, instrs):
    flat = [t for ins in instrs for t in ins]
    return struct.pack('<%dI' % (len(flat) + 2), version, len(flat) + 2, *flat)

# opcodes
OP = dict(ADD=0, AND=1, DIV=14, ELSE=18, ENDIF=21, IF=31, LT=49, MAD=50, MIN=51, MAX=52, MOV=54, MOVC=55, MUL=56,
          OR=60, DCL_TEMPS=104)

def opcode(op, length, sat=False, nz=False):
    return op | (int(sat) << 13) | (int(nz) << 18) | (length << 24)

COMP = 'xyzw'
def temp_dst(reg, mask):
    m = sum(1 << COMP.index(c) for c in mask)
    return [2 | (m << 4) | (0 << 12) | (1 << 20), reg]

def _swz(s):
    s = (s + s[-1] * 4)[:4]
    return sum(COMP.index(c) << (2 * i) for i, c in enumerate(s))

def temp_src(reg, swizzle, neg=False):
    if len(swizzle) == 1:
        tok = 2 | (2 << 2) | (COMP.index(swizzle) << 4) | (0 << 12) | (1 << 20)
    else:
        tok = 2 | (1 << 2) | (_swz(swizzle) << 4) | (0 << 12) | (1 << 20)
    if neg:
        return [tok | 0x80000000, 1 | (1 << 6), reg]
    return [tok, reg]

def cb_src(slot, index, swizzle):
    if len(swizzle) == 1:
        tok = 2 | (2 << 2) | (COMP.index(swizzle) << 4) | (8 << 12) | (2 << 20)
    else:
        tok = 2 | (1 << 2) | (_swz(swizzle) << 4) | (8 << 12) | (2 << 20)
    return [tok, slot, index]

def f2u(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]

def imm(*values):
    vals = [v if isinstance(v, int) else f2u(v) for v in values]
    if len(vals) == 1:
        return [1 | (4 << 12), vals[0]]
    assert len(vals) == 4
    return [2 | (4 << 12)] + vals

def ins(op, *operands, sat=False, nz=False):
    body = [t for o in operands for t in o]
    return [opcode(op, len(body) + 1, sat=sat, nz=nz)] + body

# ---- disassembly (d3dcompiler_47) ----
_d3d = None
def disassemble(blob):
    global _d3d
    if _d3d is None:
        _d3d = ctypes.WinDLL(r'C:\Windows\System32\d3dcompiler_47.dll')
        _d3d.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                                        ctypes.POINTER(ctypes.c_void_p)]
    out = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(blob, len(blob))
    hr = _d3d.D3DDisassemble(buf, len(blob), 0, None, ctypes.byref(out))
    if hr != 0:
        raise RuntimeError('D3DDisassemble failed 0x%08X' % (hr & 0xFFFFFFFF))
    vt = ctypes.cast(out, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    text = ctypes.string_at(get_ptr(out), get_size(out)).decode('latin1')
    release(out)
    return text

def instruction_lines(text):
    """The disassembly's instructions, one string each (from the version line on)."""
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines) if l[:3] in ('ps_', 'vs_', 'cs_', 'gs_', 'hs_', 'ds_'))
    out = []
    for l in lines[start + 1:]:
        l = l.replace(chr(0), '')
        if not l.strip() or l.lstrip().startswith('//'):
            continue
        if out and out[-1].count('{') > out[-1].count('}'):   # (the immediate constant buffer's rows)
            out[-1] += ' ' + l.strip()
        else:
            out.append(l.strip())
    return out
