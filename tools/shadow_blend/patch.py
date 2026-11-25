"""Blend between the sun's shadow cascades in the game's shadow-mask pixel shaders (ROTTR.exe's embedded DXBC).

Each shader: the pixel's light-space position r1 (cascade 0, the farthest, in its map), its margins inside cascades
1..3 (r0.xyz before `lt r0.xyz, l(0), r0.xyz`: min(extent - lateral, end - depth)), the farthest's (rF.x = end0 -
depth), the chosen cascade's map coordinates in r0.xyz after `mad r0.xyz, r1.xyz, rL, r0.xyz` (the nearest that holds
it: 3 > 2 > 1 > 0), a filter kernel reading r0.xyz alone into r0.x, then `movc o0.x, r0.w, r0.x, l(1)`.
Patched: the margins saved; the next farther cascade's coordinates and a weight t = saturate(1 - margin / (B * end))
made; the kernel run a second time on those when t > 0, and r0.x = lerp(first, second, t); in the farthest cascade the
result fades to lit (1) over its last B instead of ending in a line.

(0.9.570) The ray tracing probe's sun shadows: the pixel's view depth kept after it is made (`div r0.x, l(1), r0.x`);
before the output, while the marker M_RT_SUN is over 0.5, the probe's sun mask (t42: half the render size, .x the
sun's visibility, .w the view depth; declared here like t43, a slot under the shader's highest that no shader of the
game's declares) is read as its four texels around the pixel (bilinear), each kept only where its depth agrees with
the pixel's (|depth - z| <= 0.03 z + 3 cm). Where any is kept, the result is their weighted visibility and the pixel
counts as in range (r0.w), past the farthest cascade too; elsewhere the cascades' result stands. (0.9.571) The mask's
texel space from its own size over the render size (resinfo, cb2[46]): any scale.
"""
import re, struct, sys, os, glob, json
sys.path.insert(0, os.path.dirname(__file__))
import dxbc
from dxbc import OP, ins, temp_dst, temp_src, cb_src, imm

BLEND = 0.2          # the last fifth of each cascade blends into the next
CB = 2               # SceneBuffer's slot
PSSM_LIN = {1: 47, 2: 49, 3: 51}
PSSM_CONST = {1: 48, 2: 50, 3: 52}
PSSM_DIST = 53
M_RT_SUN = 0.7654321193695068   # (0.9.570) 1: the probe's sun mask read; 0: not (the framework writes it)
RT_SUN_SLOT = 42                 # (0.9.570) its texture slot
# (0.9.570) the opcodes the sun mask's read needs beyond dxbc.OP
OP2 = dict(RESINFO=61, ADD=0, AND=1, DIV=14, ENDIF=21, FRC=26, FTOU=28, GE=29, IADD=30, IF=31, LD=45, LT=49, MAD=50, MOV=54,
           MOVC=55, MUL=56)

def src_mod(reg, swizzle, mod):
    """(0.9.570) A temp source with a modifier (1 neg, 2 abs)."""
    t = temp_src(reg, swizzle)
    return [t[0] | 0x80000000, 1 | (mod << 6), t[-1]]

def input_src(reg, swizzle):
    return [2 | (1 << 2) | (dxbc._swz(swizzle) << 4) | (1 << 12) | (1 << 20), reg]

def resource_src(slot, swizzle):
    return [2 | (1 << 2) | (dxbc._swz(swizzle) << 4) | (7 << 12) | (1 << 20), slot]

def rt_sun(M, N, C, T, TZ):
    """(0.9.570) Before the output, the marker on: the sun mask's four texels; where any agrees, r0.x their weighted
    visibility and r0.w all ones (in range). M.xy the fraction, N.xy 1 - it, N.zw the floor (uint), C the address,
    M.z the depth tolerance, T.x / T.y the weighted visibility and the weights, T.zw a texel (.x, .w)."""
    O = OP2
    s = []
    s.append(ins(O['LT'], temp_dst(T, 'w'), imm(0.5), temp_src(TZ, 'y')))
    s.append(ins(O['IF'], temp_src(T, 'w'), nz=True))
    # (0.9.571) the mask's scale: its size (resinfo) over the render size (cb2[46].xy, ScreenResolution); was 0.5
    s.append(ins(O['RESINFO'], temp_dst(N, 'xy'), imm(0), resource_src(RT_SUN_SLOT, 'xyzw')))
    s.append(ins(O['DIV'], temp_dst(N, 'xy'), temp_src(N, 'xyxx'), cb_src(CB, 46, 'xyxx')))
    s.append(ins(O['MAD'], temp_dst(N, 'xy'), input_src(0, 'xyxx'), temp_src(N, 'xyxx'), imm(-0.5, -0.5, 0.0, 0.0)))
    s.append(ins(O['FRC'], temp_dst(M, 'xy'), temp_src(N, 'xyxx')))
    s.append(ins(O['ADD'], temp_dst(N, 'xy'), temp_src(N, 'xyxx'), src_mod(M, 'xyxx', 1)))
    s.append(ins(O['FTOU'], temp_dst(N, 'zw'), temp_src(N, 'xxxy')))
    s.append(ins(O['ADD'], temp_dst(N, 'xy'), src_mod(M, 'xyxx', 1), imm(1.0, 1.0, 0.0, 0.0)))
    s.append(ins(O['MAD'], temp_dst(M, 'z'), temp_src(TZ, 'x'), imm(0.03), imm(3.0)))
    s.append(ins(O['MOV'], temp_dst(C, 'zw'), imm(0, 0, 0, 0)))
    s.append(ins(O['MOV'], temp_dst(T, 'xy'), imm(0.0, 0.0, 0.0, 0.0)))
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        s.append(ins(O['IADD'], temp_dst(C, 'xy'), temp_src(N, 'zwzz'), imm(dx, dy, 0, 0)))
        s.append(ins(O['LD'], temp_dst(T, 'zw'), temp_src(C, 'xyzw'), resource_src(RT_SUN_SLOT, 'xxxw')))
        s.append(ins(O['ADD'], temp_dst(T, 'w'), temp_src(T, 'w'), src_mod(TZ, 'x', 1)))
        s.append(ins(O['GE'], temp_dst(T, 'w'), temp_src(M, 'z'), src_mod(T, 'w', 2)))
        s.append(ins(O['MUL'], temp_dst(M, 'w'), temp_src(M if dx else N, 'x'), temp_src(M if dy else N, 'y')))
        s.append(ins(O['AND'], temp_dst(M, 'w'), temp_src(M, 'w'), temp_src(T, 'w')))
        s.append(ins(O['MAD'], temp_dst(T, 'x'), temp_src(M, 'w'), temp_src(T, 'z'), temp_src(T, 'x')))
        s.append(ins(O['ADD'], temp_dst(T, 'y'), temp_src(T, 'y'), temp_src(M, 'w')))
    s.append(ins(O['LT'], temp_dst(T, 'z'), imm(0.0001), temp_src(T, 'y')))
    s.append(ins(O['IF'], temp_src(T, 'z'), nz=True))
    s.append(ins(O['DIV'], temp_dst(0, 'x'), temp_src(T, 'x'), temp_src(T, 'y')))
    s.append(ins(O['MOV'], temp_dst(0, 'w'), imm(0xFFFFFFFF)))
    s.append(ins(O['ENDIF']))
    s.append(ins(O['ENDIF']))
    return s

def split_operands(text):
    parts, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts

def parse(line):
    """-> (name, operands)"""
    m = re.match(r'^([a-z_0-9]+(?:\([^)]*\))*)\s*(.*)$', line)
    name, rest = m.group(1), m.group(2)
    return name, split_operands(rest) if rest else []

NO_DEST = ('if_', 'breakc', 'loop', 'endloop', 'ret', 'else', 'endif', 'break', 'discard', 'retc', 'continue', 'switch',
           'case', 'default', 'endswitch')
TWO_DEST = ('imul', 'umul', 'udiv', 'swapc', 'sincos', 'uaddc', 'usubb')
PER_COMPONENT = ('add', 'mul', 'mad', 'mov', 'movc', 'min', 'max', 'lt', 'ge', 'eq', 'ne', 'and', 'or', 'xor', 'not',
                 'ftoi', 'ftou', 'itof', 'utof', 'round_ne', 'round_ni', 'round_pi', 'round_z', 'frc', 'div', 'iadd',
                 'ige', 'ilt', 'ieq', 'ine', 'imad', 'imax', 'imin', 'ishl', 'ishr', 'ushr', 'ineg', 'exp', 'log',
                 'sqrt', 'rsq', 'rcp', 'deriv_rtx', 'deriv_rty', 'ult', 'uge', 'umax', 'umin')

def regs(operand):
    """[(reg, components)] read by an operand (its own and in its indices)."""
    return [(int(r), c) for r, c in re.findall(r'r(\d+)\.([xyzw]+)', operand)]

def dest(operand):
    m = re.match(r'^r(\d+)\.([xyzw]+)$', operand.strip())
    return (int(m.group(1)), m.group(2)) if m else None

def live_in(lines):
    """Temp components read before written in a straight list of instructions (writes in branches count)."""
    written, needed = set(), set()
    for line in lines:
        name, ops = parse(line)
        base = name.split('(')[0]
        if base.startswith(NO_DEST):
            dests, srcs = [], ops
        elif base in TWO_DEST:
            dests, srcs = ops[:2], ops[2:]
        else:
            dests, srcs = ops[:1], ops[1:]
        mask = None
        if len(dests) == 1 and dest(dests[0]):
            mask = dest(dests[0])[1]
        for s in srcs:
            for r, comps in regs(s):
                if base in PER_COMPONENT and mask and len(comps) == 4:
                    comps = ''.join(comps[dxbc.COMP.index(c)] for c in mask)
                for c in comps:
                    if (r, c) not in written:
                        needed.add((r, c))
        for d in dests:
            dd = dest(d)
            if dd:
                for c in dd[1]:
                    written.add((dd[0], c))
    return needed, written

def patch(blob):
    chunk_list = dxbc.chunks(blob)
    names = [t for t, _ in chunk_list]
    code_tag = b'SHEX' if b'SHEX' in names else b'SHDR'
    code = dict(chunk_list)[code_tag]
    version, instrs = dxbc.instructions(code)
    lines = dxbc.instruction_lines(dxbc.disassemble(blob))
    if len(lines) != len(instrs):
        raise ValueError('disassembly and tokens differ: %d / %d' % (len(lines), len(instrs)))
    find = lambda rx: [i for i, l in enumerate(lines) if re.fullmatch(rx, l)]
    i_temps = find(r'dcl_temps \d+')
    i_far = find(r'add r(\d+)\.xyzw, -r1\.wwww, cb2\[53\]\.xyzw')
    i_lt = find(r'lt r0\.xyz, l\(0\.000000, 0\.000000, 0\.000000, 0\.000000\), r0\.xyzx?')
    i_mad = find(r'mad r0\.xyz, r1\.xyzx, r\d+\.[xyzw]{4}, r0\.xyzx')
    i_ltw = find(r'lt r0\.w, l\(0\.000000\), r(\d+)\.x')
    i_end = find(r'movc o0\.x, r0\.w, r0\.x, l\(1\.000000\)')
    i_div = find(r'div r0\.x, l\(1\.000000, 1\.000000, 1\.000000, 1\.000000\), r0\.x')   # (0.9.570) the view depth
    i_t43 = find(r'dcl_resource_texture2d \(float,float,float,float\) t43')
    for n, v in (('dcl_temps', i_temps), ('far margin', i_far), ('flags', i_lt), ('cascade map', i_mad),
                 ('in range', i_ltw), ('output', i_end), ('view depth', i_div), ('t43', i_t43)):
        if len(v) != 1:
            raise ValueError('%s found %d times' % (n, len(v)))
    i_temps, i_far, i_lt, i_mad, i_ltw, i_end = i_temps[0], i_far[0], i_lt[0], i_mad[0], i_ltw[0], i_end[0]
    i_div, i_t43 = i_div[0], i_t43[0]
    if not (i_temps < i_far < i_lt < i_mad < i_end and i_ltw < i_end and i_temps < i_div < i_far):
        raise ValueError('unexpected order')
    if any(re.fullmatch(r'dcl_resource\w* .* t42(, .*)?', l) for l in lines):   # (0.9.570)
        raise ValueError('t42 already declared')
    dcl_sun = list(instrs[i_t43])
    if len(dcl_sun) != 4 or dcl_sun[2] != 43:
        raise ValueError('t43\'s declaration is not as expected')
    dcl_sun[2] = RT_SUN_SLOT
    first_code = next(i for i in range(i_temps + 1, len(lines)) if not lines[i].startswith('dcl_'))
    far = int(re.fullmatch(r'add r(\d+)\.xyzw.*', lines[i_far]).group(1))
    if int(re.fullmatch(r'lt r0\.w, l\(0\.000000\), r(\d+)\.x', lines[i_ltw]).group(1)) != far:
        raise ValueError('in-range test reads another register')
    # the far margin must still be there when the flags are made
    needed, written = live_in(lines[i_far + 1:i_lt])
    if (far, 'x') in written:
        raise ValueError('far margin overwritten before the flags')
    # the kernel: after the map, before the output; the in-range test (if there) left out of its second run
    kernel = list(range(i_mad + 1, i_end))
    kernel2 = [i for i in kernel if i != i_ltw]
    needed, written = live_in([lines[i] for i in kernel2])
    extra = sorted(r for r in needed if r not in {(0, 'x'), (0, 'y'), (0, 'z')})
    if extra:
        raise ValueError('kernel reads more than r0.xyz: %s' % extra)
    if (0, 'w') in written:
        raise ValueError('kernel writes r0.w')
    for i in kernel2:
        base = parse(lines[i])[0].split('(')[0]
        if base in ('sample', 'sample_b', 'deriv_rtx', 'deriv_rty', 'deriv_rtx_coarse', 'deriv_rty_coarse',
                    'deriv_rtx_fine', 'deriv_rty_fine', 'discard_nz', 'discard_z'):
            raise ValueError('kernel uses %s (derivatives in a branch)' % base)
    temps = instrs[i_temps][1]
    M, N, C, T = temps, temps + 1, temps + 2, temps + 3
    TZ = temps + 4   # (0.9.570) x: the pixel's view depth; y: the sun mask's marker
    out = []
    for i, tok in enumerate(instrs):
        if i == i_temps:
            out.append([tok[0], temps + 5])
            continue
        if i == first_code:   # (0.9.570)
            out.append(ins(OP2['MOV'], temp_dst(TZ, 'y'), imm(M_RT_SUN)))
        if i == i_lt:   # the margins (cascades 1..3) and the farthest's
            out.append(ins(OP['MOV'], temp_dst(M, 'xyz'), temp_src(0, 'xyzx')))
            out.append(ins(OP['MOV'], temp_dst(M, 'w'), temp_src(far, 'x')))
        if i == i_end:
            out.extend(blend_tail(M, N, C, T, [instrs[k] for k in kernel2]))
            out.extend(rt_sun(M, N, C, T, TZ))   # (0.9.570)
        out.append(tok)
        if i == i_t43:   # (0.9.570)
            out.append(dcl_sun)
        if i == i_div:
            out.append(ins(OP2['MOV'], temp_dst(TZ, 'x'), temp_src(0, 'x')))
        if i == i_mad:
            out.extend(next_cascade(M, N, C, T))
    new_code = dxbc.code_bytes(version, out)
    patched = dxbc.build([(t, new_code if t == code_tag else d) for t, d in chunk_list])
    marker = struct.pack('<f', M_RT_SUN)   # (0.9.570) the marker's byte offset (found once)
    if patched.count(marker) != 1:
        raise ValueError('the sun marker found %d times' % patched.count(marker))
    return patched, dict(kernel=len(kernel), temps=(temps, temps + 5), rt=patched.index(marker))

def next_cascade(M, N, C, T):
    """After the map: T.x = the weight, T.y = a farther cascade there; N.xyz = its map coordinates (from r1.xyz)."""
    s = []
    s.append(ins(OP['LT'], temp_dst(T, 'yzw'), imm(0.0, 0.0, 0.0, 0.0), temp_src(M, 'xxyz')))   # in cascade 1, 2, 3
    # the next farther cascade's map: 1 -> 0 (identity: Lin1.w, Const1.w), 2 -> 1, 3 -> 2
    s.append(ins(OP['MOVC'], temp_dst(N, 'xyz'), temp_src(T, 'y'), cb_src(CB, PSSM_LIN[1], 'wwww'), imm(1.0, 1.0, 1.0, 1.0)))
    s.append(ins(OP['MOVC'], temp_dst(N, 'xyz'), temp_src(T, 'z'), cb_src(CB, PSSM_LIN[1], 'xyzx'), temp_src(N, 'xyzx')))
    s.append(ins(OP['MOVC'], temp_dst(N, 'xyz'), temp_src(T, 'w'), cb_src(CB, PSSM_LIN[2], 'xyzx'), temp_src(N, 'xyzx')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'xyz'), temp_src(T, 'y'), cb_src(CB, PSSM_CONST[1], 'wwww'), imm(0.0, 0.0, 0.0, 0.0)))
    s.append(ins(OP['MOVC'], temp_dst(C, 'xyz'), temp_src(T, 'z'), cb_src(CB, PSSM_CONST[1], 'xyzx'), temp_src(C, 'xyzx')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'xyz'), temp_src(T, 'w'), cb_src(CB, PSSM_CONST[2], 'xyzx'), temp_src(C, 'xyzx')))
    s.append(ins(OP['MAD'], temp_dst(N, 'xyz'), temp_src(1, 'xyzx'), temp_src(N, 'xyzx'), temp_src(C, 'xyzx')))
    # the chosen cascade's margin and end (0: the farthest)
    s.append(ins(OP['MOVC'], temp_dst(C, 'x'), temp_src(T, 'y'), temp_src(M, 'x'), temp_src(M, 'w')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'x'), temp_src(T, 'z'), temp_src(M, 'y'), temp_src(C, 'x')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'x'), temp_src(T, 'w'), temp_src(M, 'z'), temp_src(C, 'x')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'y'), temp_src(T, 'y'), cb_src(CB, PSSM_DIST, 'y'), cb_src(CB, PSSM_DIST, 'x')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'y'), temp_src(T, 'z'), cb_src(CB, PSSM_DIST, 'z'), temp_src(C, 'y')))
    s.append(ins(OP['MOVC'], temp_dst(C, 'y'), temp_src(T, 'w'), cb_src(CB, PSSM_DIST, 'w'), temp_src(C, 'y')))
    s.append(ins(OP['OR'], temp_dst(C, 'z'), temp_src(T, 'y'), temp_src(T, 'z')))
    s.append(ins(OP['OR'], temp_dst(T, 'y'), temp_src(C, 'z'), temp_src(T, 'w')))   # a farther cascade there
    s.append(ins(OP['MUL'], temp_dst(C, 'y'), temp_src(C, 'y'), imm(BLEND)))        # the blend's width
    s.append(ins(OP['DIV'], temp_dst(C, 'x'), temp_src(C, 'x'), temp_src(C, 'y')))
    s.append(ins(OP['ADD'], temp_dst(T, 'x'), temp_src(C, 'x', neg=True), imm(1.0), sat=True))   # t
    return s

def blend_tail(M, N, C, T, kernel):
    """Before the output: r0.x = lerp(the chosen cascade's, the next one's, t); the farthest fades to lit."""
    s = []
    s.append(ins(OP['MOV'], temp_dst(C, 'x'), temp_src(0, 'x')))
    s.append(ins(OP['LT'], temp_dst(C, 'y'), imm(0.0), temp_src(T, 'x')))
    s.append(ins(OP['AND'], temp_dst(C, 'y'), temp_src(C, 'y'), temp_src(T, 'y')))
    s.append(ins(OP['IF'], temp_src(C, 'y'), nz=True))
    s.append(ins(OP['MOV'], temp_dst(0, 'xyz'), temp_src(N, 'xyzx')))
    s.extend(kernel)
    s.append(ins(OP['ADD'], temp_dst(0, 'x'), temp_src(0, 'x'), temp_src(C, 'x', neg=True)))
    s.append(ins(OP['MAD'], temp_dst(C, 'x'), temp_src(T, 'x'), temp_src(0, 'x'), temp_src(C, 'x')))
    s.append(ins(OP['ELSE']))
    s.append(ins(OP['ADD'], temp_dst(0, 'x'), temp_src(C, 'x', neg=True), imm(1.0)))
    s.append(ins(OP['MAD'], temp_dst(C, 'x'), temp_src(T, 'x'), temp_src(0, 'x'), temp_src(C, 'x')))
    s.append(ins(OP['ENDIF']))
    s.append(ins(OP['MOV'], temp_dst(0, 'x'), temp_src(C, 'x')))
    return s

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    report = {}
    for name in sys.argv[3:]:
        blob = open(os.path.join(src, name + '.dxbc'), 'rb').read()
        try:
            patched, info = patch(blob)
            open(os.path.join(out, name + '.dxbc'), 'wb').write(patched)
            open(os.path.join(out, name + '.asm'), 'w').write(dxbc.disassemble(patched).replace('\x00', ''))
            report[name] = dict(ok=True, size=(len(blob), len(patched)), **info)
        except Exception as e:
            report[name] = dict(ok=False, error=str(e))
        print(name, report[name])
