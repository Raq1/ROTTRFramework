"""The lights pass, patched (Framework 0.9.526): the game's tiled lighting pixel shaders (TiledLightingPS, ROTTR.exe's
embedded DXBC, 32 variants) with
  1. softer lamp shadows (0.9.525's lamp_patch.py, now here): the spot lights' four bilinear comparison taps half a
     texel apart run at four centres moved by (+-1, +-1) x the centre step and averaged: a 4 x 4 grid. The block's own
     offsets use the inner step. Inner = centre = 1 / 1.5 / 2 texels: Soft / Softer / Softest; inner 1, centre 0: four
     copies of the game's own block, the game's result (lamp softness off, contact shadows on).
  2. contact shadows: for the sun (the directional lights), point and capsule lights and spot lights (not box lights),
     a short ray from the pixel toward the light through the depth buffer; what lies in front of it within a few cm
     takes that light's diffuse and specular away (its share of the pixel's sums).

The variants: MSAA or not (the depth and normals read with ldms, sample 0), the normals packed or not (the view normal
in r0.yzw or r0.xyz), the lights' data from t54 (STiledLightData, 384 bytes) or cb12 (STiledLightHotData, 12 registers:
+0 position, type in .w; +1 colour; +8 direction). The prologue (checked in each): r1.xyz = the pixel's view-space
position P (z forward: z = 1 / (d x cb2[20].x + cb2[20].y) from the depth t1; x = z x (uv.x x cb2[18].x + cb2[18].z) -
(cb2[85].x z + cb2[85].y) cb2[85].z, y = z x (uv.y x cb2[18].y + cb2[18].w), uv = v0.xy x cb2[45].zw + cb2[45].xy), the
view normal in r0; both kept to the end. Each light's sums: `mad A, s, colour, A` (2 or 3 together: diffuse, its
luminance, specular); colour = cb12[i + 1] or a temp loaded from it or from t54 (+64).

The trace (game units: cm): from O = P + N x (0.3 + 0.002 z) along L (to the light's position, or -direction for the
sun), STEPS samples at t = R ((k + j) / STEPS)^2 (j: interleaved gradient noise of the pixel), each projected back to a
pixel (skipped while it has not left the pixel) and the depth there read: a hit when the sample is behind the scene by
more than 0.2 + 0.001 z and less than 5 + 0.01 z (thicker things in front are not its occluders). Occlusion = the
largest 1 - t / R of the hits; visibility = 1 - strength x occlusion. Only where N.L > 0. The sums' factors s are
multiplied by it (copies: the factors themselves are left as they are).

Markers (floats found once each in the patched shader; the framework writes the chosen values): the contact shadows'
strength and ray length (cm), the lamp steps (inner, centre, in texels).

(0.9.561) 3. the ray tracing probe's lamp shadows: after each light's contact trace, while the marker M_RT is over 0.5,
the light's position (cb12 +0 / t54 +0) is looked up in the probe's table (t46, a 4 x 1 texture: in texels 0 .. 2 a
lamp's view position as the lights' pass has it, and 1 when traced); on a match, the probe's mask (t45, half the render
size: three lamps' visibility and the view depth) is read at the pixel / 2, the matched lamp's channel taken, and kept
only where the mask's depth agrees with the pixel's (|depth - P.z| <= 0.03 P.z + 3 cm; else 1): the light's visibility
is multiplied by it. t45 and t46 are declared here: slots under the shader's highest, t58 (the engine copies that many
descriptors), that no shader of the game's declares; the probe puts its descriptors there for the lights' pass.
(0.9.561 used t50 and a buffer at t51; t51 is some scene shaders' own, so the engine's slot held their descriptors.)
(0.9.571) Its texel space from its own size over the render size (resinfo, cb2[46]), any scale.
(0.9.564) The mask is read as its four texels around the pixel (bilinear), each kept only where its depth agrees, the
kept ones' weights renormalised (none kept: 1): smooth edges across the half-size mask, no light through silhouettes.
"""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import dxbc
from dxbc import ins, temp_dst, temp_src, cb_src, imm, COMP, _swz
from patch import parse, live_in

M_INNER = 1.2345678806304932     # 0x3F9E0652
M_CENTRE = 2.3456788063049316    # 0x40161F9A
M_STRENGTH = 0.8765432238578796  # 0x3F6064E7
M_LENGTH = 17.654321670532227    # 0x418D3C0C
M_DEBUG = 0.43210986256599426    # (0.9.537) 1: the debug view (only the contact shadows, in red); 0: off
M_RT = 0.6543210744857788        # (0.9.561) 1: the probe's ray-traced lamp shadows read (t45, t46); 0: off
RT_MASK, RT_TABLE = 45, 46       # (0.9.561) the probe's mask and lamp table; (0.9.562) t45 / t46 (was t50 / t51: t51 is
                                 # some scene shaders' own; none of the exe's declares t45 or t46)
CB, TEXEL = 2, 71
STEPS = 16   # (0.9.532: 12, was 8; 0.9.533: 16)
JITTER = False   # (0.9.533: no per-pixel noise)
SCREEN_REACH = 0.15   # (0.9.535: 0.05; 0.9.536: 0.15) a ray at most this share of the screen's height long on screen
THICK_FAR = (10.0, 0.02)   # (0.9.535: 2.5; 0.9.536: 4; 0.9.539: 10 + 0.02 z) nothing counts past a + b z cm behind the
                           # scene. (0.9.539) At 4 cm a jaw 6 cm before the neck did not count where its front was seen, only
                           # along its underside's sliver at a grazing angle, which the samples hit or missed row by row: lines
THICK_FADE = (5.0, 0.01)   # (0.9.535: 1; 0.9.536: 3, gradual; 0.9.539: 5 + 0.01 z) the count fades to nothing over the last
                           # a + b z cm before that
NORMAL_FREE = True   # (0.9.538) the trace takes nothing from the G-buffer's normal (no N.L test, its start toward the camera)
REFINE = 5   # (0.9.539) halvings between the last miss and the first full hit (0: none)
END_FADE = 0.3   # (0.9.539) the hits' weight fades to nothing over this share of the ray's length used (0: none)

OP = dict(RESINFO=61, ADD=0, AND=1, BREAKC=3, DIV=14, DP2=15, DP3=16, ENDIF=21, ENDLOOP=22, EQ=24, FRC=26, FTOI=27, FTOU=28, GE=29,
          IADD=30, IEQ=32, IGE=33, IF=31, INE=39, LD=45, LOOP=48, LT=49, MAD=50, MIN=51, MAX=52, MOV=54, MOVC=55, MUL=56,
          OR=60, RSQ=68, SQRT=75, UGE=80, UTOF=86)

# ---- operands
def src_mod(reg, swizzle, mod):
    """A temp source with a modifier (1 neg, 2 abs)."""
    t = temp_src(reg, swizzle)
    return [t[0] | 0x80000000, 1 | (mod << 6), t[-1]]

def neg(operand):
    return [operand[0] | 0x80000000, 1 | (1 << 6)] + operand[1:]

def output_dst(reg, mask):
    return [2 | (sum(1 << COMP.index(c) for c in mask) << 4) | (2 << 12) | (1 << 20), reg]

def input_src(reg, swizzle):
    return [2 | (1 << 2) | (_swz(swizzle) << 4) | (1 << 12) | (1 << 20), reg]

def resource_src(slot, swizzle):
    return [2 | (1 << 2) | (_swz(swizzle) << 4) | (7 << 12) | (1 << 20), slot]

def cb12_src(index_reg, index_comp, offset, swizzle):
    """cb12[r.c + offset].swizzle (STiledLightHotData: 12 registers a light)."""
    tok = 2 | (1 << 2) | (_swz(swizzle) << 4) | (8 << 12) | (2 << 20) | (0 << 22) | (3 << 25)
    rel = [2 | (2 << 2) | (COMP.index(index_comp) << 4) | (0 << 12) | (1 << 20), index_reg]
    return [tok, 12, offset] + rel

def operand_len(t, i):
    tok = t[i]
    n = 1
    ext = tok >> 31
    while ext:
        ext = t[i + n] >> 31
        n += 1
    otype = (tok >> 12) & 0xFF
    ncomp = tok & 3
    if otype == 4:
        n += 1 if ncomp == 1 else 4
    elif otype == 5:
        n += 2 if ncomp == 1 else 8
    dim = (tok >> 20) & 3
    for d in range(dim):
        rep = (tok >> (22 + 3 * d)) & 7
        if rep == 0:
            n += 1
        elif rep == 1:
            n += 2
        elif rep == 2:
            n += operand_len(t, i + n)
        elif rep == 3:
            n += 1
            n += operand_len(t, i + n)
        else:
            raise ValueError('index representation %d' % rep)
    return n

def split_instruction(t):
    """-> (opcode tokens incl. extended, [operand token lists])"""
    n = 1
    ext = t[0] >> 31
    while ext:
        ext = t[n] >> 31
        n += 1
    head, ops, i = t[:n], [], n
    while i < len(t):
        k = operand_len(t, i)
        ops.append(t[i:i + k])
        i += k
    return head, ops

def join_instruction(head, ops):
    body = [x for o in ops for x in o]
    head = list(head)
    head[0] = (head[0] & ~(0x7F << 24)) | ((len(head) + len(body)) << 24)
    return head + body

def ld_structured(template, dst_reg, mask, offset, swizzle):
    """A load from t54 like the template (its index operand), into dst_reg.mask from byte offset."""
    head, ops = split_instruction(template)
    if len(ops) != 4 or ops[3][-1] != 54:
        raise ValueError('not a t54 load')
    return join_instruction(head, [temp_dst(dst_reg, mask), ops[1], imm(offset), resource_src(54, swizzle)])

def ld_depth(template, reg):
    """reg.x = the depth texture t1 at reg.xyzw (int; z, w 0), like the prologue's load (ld or ldms: its sample)."""
    head, ops = split_instruction(template)
    if len(ops) < 3 or ops[2][-1] != 1:
        raise ValueError('not the depth load')
    return join_instruction(head, [temp_dst(reg, 'x'), temp_src(reg, 'xyzw'), resource_src(1, 'xyzw')] + ops[3:])

# ---- the trace
def light_data(source, T0, T1, T2):
    """T0.x = the light's type, T1.xyz = its position, T2.xyz = its direction (where it goes)."""
    kind, data = source
    if kind == 't54':
        return [ld_structured(data, T0, 'x', 140, 'xxxx'), ld_structured(data, T1, 'xyz', 0, 'xyzx'),
                ld_structured(data, T2, 'xyz', 16, 'xyzx')]
    reg, comp = data
    return [ins(OP['MOV'], temp_dst(T0, 'x'), cb12_src(reg, comp, 0, 'wwww')),
            ins(OP['MOV'], temp_dst(T1, 'xyz'), cb12_src(reg, comp, 0, 'xyzx')),
            ins(OP['MOV'], temp_dst(T2, 'xyz'), cb12_src(reg, comp, 8, 'xyzx'))]

def project(T3, T6):
    """T6.xy = the pixel of Q (T3.xyz) (the prologue's mapping, inverted)."""
    return [ins(OP['MAD'], temp_dst(T6, 'x'), cb_src(CB, 85, 'x'), temp_src(T3, 'z'), cb_src(CB, 85, 'y')),
            ins(OP['MAD'], temp_dst(T6, 'x'), temp_src(T6, 'x'), cb_src(CB, 85, 'z'), temp_src(T3, 'x')),
            ins(OP['MOV'], temp_dst(T6, 'y'), temp_src(T3, 'y')),
            ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), temp_src(T3, 'zzzz')),
            ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), neg(cb_src(CB, 18, 'zwzz'))),
            ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), cb_src(CB, 18, 'xyxx')),
            ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), neg(cb_src(CB, 45, 'xyxx'))),
            ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), cb_src(CB, 45, 'zwzz'))]

def hit_weights(depth_load, T3, T4, T6):
    """The depth at Q's pixel (T6.xy) read: T4.z = how far Q is behind the scene, T4.w = the entry weight, T6.y = the
    thickness weight (both 0..1)."""
    s = [ins(OP['FTOI'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx')),
         ins(OP['MOV'], temp_dst(T6, 'zw'), imm(0, 0, 0, 0)),
         depth_load,
         ins(OP['MAD'], temp_dst(T6, 'x'), temp_src(T6, 'x'), cb_src(CB, 20, 'x'), cb_src(CB, 20, 'y')),
         ins(OP['DIV'], temp_dst(T6, 'x'), imm(1.0), temp_src(T6, 'x')),                  # the scene's z there
         ins(OP['ADD'], temp_dst(T4, 'z'), temp_src(T3, 'z'), src_mod(T6, 'x', 1))]       # behind it by
    # (0.9.532) a hit counts by degrees: from nothing at the bias to fully 1 + 0.004 z cm past it, and back to nothing
    # over the last THICK_FADE cm before the thickness (it was all or nothing: hard, dithered edges)
    s += [ins(OP['MAD'], temp_dst(T4, 'w'), temp_src(T3, 'z'), imm(0.001), imm(0.2)),
          ins(OP['ADD'], temp_dst(T4, 'w'), temp_src(T4, 'z'), src_mod(T4, 'w', 1)),
          ins(OP['MAD'], temp_dst(T6, 'y'), temp_src(T3, 'z'), imm(0.004), imm(1.0)),
          ins(OP['DIV'], temp_dst(T4, 'w'), temp_src(T4, 'w'), temp_src(T6, 'y'), sat=True)]
    s += [ins(OP['MAD'], temp_dst(T6, 'y'), temp_src(T3, 'z'), imm(THICK_FAR[1]), imm(THICK_FAR[0])),
          ins(OP['ADD'], temp_dst(T6, 'y'), temp_src(T6, 'y'), src_mod(T4, 'z', 1)),
          ins(OP['MAD'], temp_dst(T6, 'z'), temp_src(T3, 'z'), imm(THICK_FADE[1]), imm(THICK_FADE[0])),
          ins(OP['DIV'], temp_dst(T6, 'y'), temp_src(T6, 'y'), temp_src(T6, 'z'), sat=True)]
    return s

def full_hit(T0, T4, T6):
    """T0.x = all ones if the entry weight is full and the thickness weight above nothing (a full hit), else 0."""
    return [ins(OP['GE'], temp_dst(T0, 'x'), temp_src(T4, 'w'), imm(1.0)),
            ins(OP['LT'], temp_dst(T0, 'y'), imm(0.0), temp_src(T6, 'y')),
            ins(OP['AND'], temp_dst(T0, 'x'), temp_src(T0, 'x'), temp_src(T0, 'y'))]

def falloff(T0, T3, T6, TS):
    """T6.x = (1 - t / R)^2: nearer occluders darker; (0.9.536) R the chosen length, not the capped one (a capped ray's
    near hits lost most of their weight: the inside of the mouth lit). (0.9.539) Faded out over the last END_FADE of
    the length used (T0.w): a capped ray's last hits still weighed (1 - R' / R)^2, so the shadow ended in a hard line
    where the occluder left its reach (the cap follows the camera's distance: the line moved with it). Uses T6.z."""
    s = [ins(OP['DIV'], temp_dst(T6, 'x'), temp_src(T3, 'w'), temp_src(TS, 'y')),
         ins(OP['ADD'], temp_dst(T6, 'x'), src_mod(T6, 'x', 1), imm(1.0)),
         ins(OP['MUL'], temp_dst(T6, 'x'), temp_src(T6, 'x'), temp_src(T6, 'x'))]
    if END_FADE:
        s += [ins(OP['DIV'], temp_dst(T6, 'z'), temp_src(T3, 'w'), temp_src(T0, 'w')),
              ins(OP['MAD'], temp_dst(T6, 'z'), src_mod(T6, 'z', 1), imm(1.0 / END_FADE), imm(1.0 / END_FADE), sat=True),
              ins(OP['MUL'], temp_dst(T6, 'x'), temp_src(T6, 'x'), temp_src(T6, 'z'))]
    return s

def contact_trace(source, depth_load, normal, T, TS, TV):
    """T[5].x = the light's visibility (1: no contact shadow)."""
    T0, T1, T2, T3, T4, T5, T6 = T
    s = []
    s.append(ins(OP['MOV'], temp_dst(T5, 'x'), imm(1.0)))
    s.extend(light_data(source, T0, T1, T2))
    s.append(ins(OP['INE'], temp_dst(T0, 'y'), temp_src(T0, 'x'), imm(4)))     # not a box light
    s.append(ins(OP['ADD'], temp_dst(T1, 'xyz'), temp_src(T1, 'xyzx'), src_mod(1, 'xyzx', 1)))
    s.append(ins(OP['DP3'], temp_dst(T1, 'w'), temp_src(T1, 'xyzx'), temp_src(T1, 'xyzx')))
    s.append(ins(OP['RSQ'], temp_dst(T1, 'w'), temp_src(T1, 'w')))
    s.append(ins(OP['MUL'], temp_dst(T1, 'xyz'), temp_src(T1, 'xyzx'), temp_src(T1, 'wwww')))
    s.append(ins(OP['IEQ'], temp_dst(T0, 'z'), temp_src(T0, 'x'), imm(5)))     # directional
    s.append(ins(OP['MOVC'], temp_dst(T1, 'xyz'), temp_src(T0, 'zzzz'), src_mod(T2, 'xyzx', 1), temp_src(T1, 'xyzx')))
    if not NORMAL_FREE:
        s.append(ins(OP['DP3'], temp_dst(T0, 'w'), temp_src(0, normal), temp_src(T1, 'xyzx')))     # N.L
        s.append(ins(OP['LT'], temp_dst(T0, 'z'), imm(0.0), temp_src(T0, 'w')))
        s.append(ins(OP['AND'], temp_dst(T0, 'y'), temp_src(T0, 'y'), temp_src(T0, 'z')))
    s.append(ins(OP['LT'], temp_dst(T0, 'z'), imm(0.0), temp_src(TS, 'x')))  # strength > 0
    s.append(ins(OP['AND'], temp_dst(T0, 'y'), temp_src(T0, 'y'), temp_src(T0, 'z')))
    s.append(ins(OP['IF'], temp_src(T0, 'y'), nz=True))
    # the ray's origin, off the surface
    s.append(ins(OP['MAD'], temp_dst(T0, 'w'), temp_src(1, 'z'), imm(0.002), imm(0.3)))
    if NORMAL_FREE:
        # (0.9.538) toward the camera along the pixel's own line of sight (O = P (1 - d / z)): no sideways shift, nothing
        # of the normal map (its pores flipped the N.L test and moved the start: speckles where the skin turns from the
        # light, the debug view's grainy edge under the jaw)
        s.append(ins(OP['DIV'], temp_dst(T0, 'w'), temp_src(T0, 'w'), temp_src(1, 'z')))
        s.append(ins(OP['ADD'], temp_dst(T0, 'w'), src_mod(T0, 'w', 1), imm(1.0)))
        s.append(ins(OP['MUL'], temp_dst(T2, 'xyz'), temp_src(1, 'xyzx'), temp_src(T0, 'wwww')))
    else:
        s.append(ins(OP['MAD'], temp_dst(T2, 'xyz'), temp_src(0, normal), temp_src(T0, 'wwww'), temp_src(1, 'xyzx')))
    if JITTER:
        # jitter: interleaved gradient noise
        s.append(ins(OP['DP2'], temp_dst(T2, 'w'), input_src(0, 'xyxx'), imm(0.06711056, 0.00583715, 0.0, 0.0)))
        s.append(ins(OP['FRC'], temp_dst(T2, 'w'), temp_src(T2, 'w')))
        s.append(ins(OP['MUL'], temp_dst(T2, 'w'), temp_src(T2, 'w'), imm(52.9829189)))
        s.append(ins(OP['FRC'], temp_dst(T2, 'w'), temp_src(T2, 'w')))
        # (0.9.532) moved each frame by the projection's offset (cb2[18].zw: it carries the temporal AA's sub-pixel
        # jitter, if any), so a temporal AA averages the pattern out instead of leaving it as a fixed dither
        s.append(ins(OP['DP2'], temp_dst(T6, 'x'), cb_src(CB, 18, 'zwzz'), imm(97.13, 61.71, 0.0, 0.0)))
        s.append(ins(OP['FRC'], temp_dst(T6, 'x'), temp_src(T6, 'x')))
        s.append(ins(OP['ADD'], temp_dst(T2, 'w'), temp_src(T2, 'w'), temp_src(T6, 'x')))
        s.append(ins(OP['FRC'], temp_dst(T2, 'w'), temp_src(T2, 'w')))
    else:
        # (0.9.533) no noise: every pixel samples at the same places along its ray (DLAA turned the noise, fixed or
        # moving, into blotches); the soft hits keep the steps between samples smooth
        s.append(ins(OP['MOV'], temp_dst(T2, 'w'), imm(0.5)))
    # (0.9.535) the ray at most SCREEN_REACH of the screen's height long on screen: in a close-up a fixed length in cm
    # crossed half the screen, behind unrelated things a few cm nearer the camera (the collar, the jaw), and whether each
    # sample counted flipped with their exact depth: blotches. T0.w = the length used.
    # (0.9.536) measured on a short first stretch, d = 5 % of the depth (the whole ray's end was projected: toward the
    # camera it reached z <= 0 and the cap cut the ray to nothing), scaled up
    s.append(ins(OP['MUL'], temp_dst(T3, 'w'), temp_src(T2, 'z'), imm(0.05)))
    s.append(ins(OP['MAD'], temp_dst(T3, 'xyz'), temp_src(T1, 'xyzx'), temp_src(T3, 'wwww'), temp_src(T2, 'xyzx')))
    s.append(ins(OP['MAD'], temp_dst(T6, 'x'), cb_src(CB, 85, 'x'), temp_src(T3, 'z'), cb_src(CB, 85, 'y')))
    s.append(ins(OP['MAD'], temp_dst(T6, 'x'), temp_src(T6, 'x'), cb_src(CB, 85, 'z'), temp_src(T3, 'x')))
    s.append(ins(OP['MOV'], temp_dst(T6, 'y'), temp_src(T3, 'y')))
    s.append(ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), temp_src(T3, 'zzzz')))
    s.append(ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), neg(cb_src(CB, 18, 'zwzz'))))
    s.append(ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), cb_src(CB, 18, 'xyxx')))
    s.append(ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), neg(cb_src(CB, 45, 'xyxx'))))
    s.append(ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), cb_src(CB, 45, 'zwzz')))
    s.append(ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), neg(input_src(0, 'xyxx'))))
    s.append(ins(OP['DP2'], temp_dst(T6, 'x'), temp_src(T6, 'xyxx'), temp_src(T6, 'xyxx')))
    s.append(ins(OP['SQRT'], temp_dst(T6, 'x'), temp_src(T6, 'x')))                             # d's length in pixels
    s.append(ins(OP['DIV'], temp_dst(T6, 'y'), imm(SCREEN_REACH), cb_src(CB, 45, 'w')))         # the most, in pixels
    s.append(ins(OP['MUL'], temp_dst(T6, 'y'), temp_src(T6, 'y'), temp_src(T3, 'w')))
    s.append(ins(OP['DIV'], temp_dst(T6, 'z'), temp_src(T6, 'y'), temp_src(T6, 'x')))          # the cm that reach it
    s.append(ins(OP['MIN'], temp_dst(T0, 'w'), temp_src(T6, 'z'), temp_src(TS, 'y')))
    s.append(ins(OP['MOV'], temp_dst(T4, 'xy'), imm(0, 0, 0, 0)))             # k = 0, occlusion = 0
    if REFINE:
        s.append(ins(OP['MOV'], temp_dst(T5, 'yzw'), imm(0, 0, 0, 0)))      # (0.9.539) before / at the first full hit; found
    s.append(ins(OP['LOOP']))
    s.append(ins(OP['UGE'], temp_dst(T4, 'w'), temp_src(T4, 'x'), imm(STEPS)))
    s.append(ins(OP['BREAKC'], temp_src(T4, 'w'), nz=True))
    s.append(ins(OP['UTOF'], temp_dst(T4, 'w'), temp_src(T4, 'x')))
    s.append(ins(OP['ADD'], temp_dst(T4, 'w'), temp_src(T4, 'w'), temp_src(T2, 'w')))
    s.append(ins(OP['MUL'], temp_dst(T4, 'w'), temp_src(T4, 'w'), imm(1.0 / STEPS)))
    s.append(ins(OP['MUL'], temp_dst(T4, 'w'), temp_src(T4, 'w'), temp_src(T4, 'w')))
    s.append(ins(OP['MUL'], temp_dst(T3, 'w'), temp_src(T4, 'w'), temp_src(T0, 'w')))   # t (0.9.535: of the length used)
    s.append(ins(OP['MAD'], temp_dst(T3, 'xyz'), temp_src(T1, 'xyzx'), temp_src(T3, 'wwww'), temp_src(T2, 'xyzx')))  # Q
    s.extend(project(T3, T6))
    # left the pixel?
    s.append(ins(OP['ADD'], temp_dst(T4, 'zw'), temp_src(T6, 'xxxy'), neg(input_src(0, 'xxxy'))))
    s.append(ins(OP['MAX'], temp_dst(T4, 'z'), src_mod(T4, 'z', 2), src_mod(T4, 'w', 2)))
    s.append(ins(OP['GE'], temp_dst(T4, 'z'), temp_src(T4, 'z'), imm(1.0)))
    s.append(ins(OP['IF'], temp_src(T4, 'z'), nz=True))
    s.extend(hit_weights(depth_load, T3, T4, T6))
    if REFINE:
        # (0.9.539) the first sample fully in (entry 1, inside the thickness): t_hi = its t (T5.z), t_lo = the last
        # sample before it (T5.y), found (T5.w)
        s.extend(full_hit(T0, T4, T6))
        s.append(ins(OP['MOVC'], temp_dst(T0, 'y'), temp_src(T0, 'x'), temp_src(T3, 'w'), temp_src(T5, 'z')))
        s.append(ins(OP['MOVC'], temp_dst(T5, 'z'), temp_src(T5, 'w'), temp_src(T5, 'z'), temp_src(T0, 'y')))
        s.append(ins(OP['OR'], temp_dst(T0, 'x'), temp_src(T0, 'x'), temp_src(T5, 'w')))
        s.append(ins(OP['MOVC'], temp_dst(T5, 'y'), temp_src(T0, 'x'), temp_src(T5, 'y'), temp_src(T3, 'w')))
        s.append(ins(OP['MOV'], temp_dst(T5, 'w'), temp_src(T0, 'x')))
    s.append(ins(OP['MUL'], temp_dst(T4, 'w'), temp_src(T4, 'w'), temp_src(T6, 'y')))
    s.extend(falloff(T0, T3, T6, TS))
    s.append(ins(OP['MUL'], temp_dst(T6, 'x'), temp_src(T6, 'x'), temp_src(T4, 'w')))
    s.append(ins(OP['MAX'], temp_dst(T4, 'y'), temp_src(T4, 'y'), temp_src(T6, 'x')))
    s.append(ins(OP['ENDIF']))
    s.append(ins(OP['IADD'], temp_dst(T4, 'x'), temp_src(T4, 'x'), imm(1)))
    s.append(ins(OP['ENDLOOP']))
    if REFINE:
        # (0.9.539) The samples sit at fixed distances along the ray, so where an occluder begins was only known to the
        # nearest sample: the shadow stepped from one sample's weight to the next as the occluder's edge passed them
        # (lines along the jaw on the neck, moving with the camera's distance: the cap changes where the samples fall).
        # Halving the gap between the last miss and the first full hit REFINE times finds the edge to 1 / 2^REFINE of a
        # step; its weight there joins the maximum, so the shadow follows the edge smoothly.
        s.append(ins(OP['IF'], temp_src(T5, 'w'), nz=True))
        s.append(ins(OP['MOV'], temp_dst(T4, 'x'), imm(0)))
        s.append(ins(OP['LOOP']))
        s.append(ins(OP['UGE'], temp_dst(T4, 'w'), temp_src(T4, 'x'), imm(REFINE)))
        s.append(ins(OP['BREAKC'], temp_src(T4, 'w'), nz=True))
        s.append(ins(OP['ADD'], temp_dst(T3, 'w'), temp_src(T5, 'y'), temp_src(T5, 'z')))
        s.append(ins(OP['MUL'], temp_dst(T3, 'w'), temp_src(T3, 'w'), imm(0.5)))
        s.append(ins(OP['MAD'], temp_dst(T3, 'xyz'), temp_src(T1, 'xyzx'), temp_src(T3, 'wwww'), temp_src(T2, 'xyzx')))
        s.extend(project(T3, T6))
        s.extend(hit_weights(depth_load, T3, T4, T6))
        s.extend(full_hit(T0, T4, T6))
        s.append(ins(OP['MOVC'], temp_dst(T5, 'z'), temp_src(T0, 'x'), temp_src(T3, 'w'), temp_src(T5, 'z')))
        s.append(ins(OP['MOVC'], temp_dst(T5, 'y'), temp_src(T0, 'x'), temp_src(T5, 'y'), temp_src(T3, 'w')))
        s.extend(falloff(T0, T3, T6, TS))
        s.append(ins(OP['MUL'], temp_dst(T6, 'x'), temp_src(T6, 'x'), temp_src(T6, 'y')))     # (entry 1 when full)
        s.append(ins(OP['AND'], temp_dst(T6, 'x'), temp_src(T6, 'x'), temp_src(T0, 'x')))     # only a full hit's
        s.append(ins(OP['MAX'], temp_dst(T4, 'y'), temp_src(T4, 'y'), temp_src(T6, 'x')))
        s.append(ins(OP['IADD'], temp_dst(T4, 'x'), temp_src(T4, 'x'), imm(1)))
        s.append(ins(OP['ENDLOOP']))
        s.append(ins(OP['ENDIF']))
    s.append(ins(OP['MUL'], temp_dst(T4, 'y'), temp_src(T4, 'y'), temp_src(TS, 'x')))
    s.append(ins(OP['ADD'], temp_dst(T5, 'x'), src_mod(T4, 'y', 1), imm(1.0)))
    s.append(ins(OP['ENDIF']))
    s.append(ins(OP['MIN'], temp_dst(TV, 'x'), temp_src(TV, 'x'), temp_src(T5, 'x')))   # (0.9.537) the debug view's
    return s

def rt_visibility(source, T, TR):
    """(0.9.561) T[5].x *= the probe's ray-traced visibility of this light (its table and mask; see the top)."""
    T0, T1, T2, T3, T4, T5, T6 = T
    s = []
    s.append(ins(OP['LT'], temp_dst(T3, 'w'), imm(0.5), temp_src(TR, 'x')))
    s.append(ins(OP['IF'], temp_src(T3, 'w'), nz=True))
    s.extend(light_data(source, T0, T1, T2))                                   # T1.xyz: the light's view position
    s.append(ins(OP['MOV'], temp_dst(T4, 'x'), imm(0xFFFFFFFF)))               # its channel (-1: not in the table)
    for k in range(3):
        s.append(ins(OP['LD'], temp_dst(T2, 'xyzw'), imm(k, 0, 0, 0), resource_src(RT_TABLE, 'xyzw')))   # texel (k, 0)
        s.append(ins(OP['EQ'], temp_dst(T3, 'xyz'), temp_src(T1, 'xyzx'), temp_src(T2, 'xyzx')))
        s.append(ins(OP['AND'], temp_dst(T3, 'x'), temp_src(T3, 'x'), temp_src(T3, 'y')))
        s.append(ins(OP['AND'], temp_dst(T3, 'x'), temp_src(T3, 'x'), temp_src(T3, 'z')))
        s.append(ins(OP['LT'], temp_dst(T3, 'y'), imm(0.5), temp_src(T2, 'w')))
        s.append(ins(OP['AND'], temp_dst(T3, 'x'), temp_src(T3, 'x'), temp_src(T3, 'y')))
        s.append(ins(OP['MOVC'], temp_dst(T4, 'x'), temp_src(T3, 'x'), imm(k), temp_src(T4, 'x')))
    s.append(ins(OP['IGE'], temp_dst(T3, 'x'), temp_src(T4, 'x'), imm(0)))
    s.append(ins(OP['IF'], temp_src(T3, 'x'), nz=True))
    # (0.9.564) the mask read as four texels, bilinear, each kept only where its depth agrees with the pixel's (0.9.561 .. 3:
    # the nearest one texel). T3.xyz the matched lamp's channel as a 0 / 1 mask; T4.x the weighted visibility, T4.y the
    # weights; m = pixel / 2 - 0.5 in the mask's texels: T0.xy its fraction, T6.xy 1 - it, T6.zw its floor; T0.z the
    # depth tolerance (0.03 P.z + 3 cm); none kept: 1
    s.append(ins(OP['IEQ'], temp_dst(T3, 'xyz'), temp_src(T4, 'xxxx'), imm(0, 1, 2, 0)))
    s.append(ins(OP['AND'], temp_dst(T3, 'xyz'), temp_src(T3, 'xyzx'), imm(1.0, 1.0, 1.0, 0.0)))
    s.append(ins(OP['MOV'], temp_dst(T4, 'xy'), imm(0.0, 0.0, 0.0, 0.0)))
    # (0.9.571) the mask's scale: its size (resinfo) over the render size (SceneBuffer's ScreenResolution, cb2[46].xy);
    # it was 0.5 (half the render size) until the probe capped its masks at 1280 wide
    s.append(ins(OP['RESINFO'], temp_dst(T6, 'xy'), imm(0), resource_src(RT_MASK, 'xyzw')))
    s.append(ins(OP['DIV'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), cb_src(CB, 46, 'xyxx')))
    s.append(ins(OP['MAD'], temp_dst(T6, 'xy'), input_src(0, 'xyxx'), temp_src(T6, 'xyxx'), imm(-0.5, -0.5, 0.0, 0.0)))
    s.append(ins(OP['FRC'], temp_dst(T0, 'xy'), temp_src(T6, 'xyxx')))
    s.append(ins(OP['ADD'], temp_dst(T6, 'xy'), temp_src(T6, 'xyxx'), src_mod(T0, 'xyxx', 1)))
    s.append(ins(OP['FTOU'], temp_dst(T6, 'zw'), temp_src(T6, 'xxxy')))
    s.append(ins(OP['ADD'], temp_dst(T6, 'xy'), src_mod(T0, 'xyxx', 1), imm(1.0, 1.0, 0.0, 0.0)))
    s.append(ins(OP['MAD'], temp_dst(T0, 'z'), temp_src(1, 'z'), imm(0.03), imm(3.0)))
    s.append(ins(OP['MOV'], temp_dst(T1, 'zw'), imm(0, 0, 0, 0)))
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        s.append(ins(OP['IADD'], temp_dst(T1, 'xy'), temp_src(T6, 'zwzz'), imm(dx, dy, 0, 0)))
        s.append(ins(OP['LD'], temp_dst(T2, 'xyzw'), temp_src(T1, 'xyzw'), resource_src(RT_MASK, 'xyzw')))
        s.append(ins(OP['DP3'], temp_dst(T2, 'x'), temp_src(T2, 'xyzx'), temp_src(T3, 'xyzx')))
        s.append(ins(OP['ADD'], temp_dst(T2, 'y'), temp_src(T2, 'w'), src_mod(1, 'z', 1)))
        s.append(ins(OP['GE'], temp_dst(T2, 'y'), temp_src(T0, 'z'), src_mod(T2, 'y', 2)))
        s.append(ins(OP['MUL'], temp_dst(T2, 'z'), temp_src(T0 if dx else T6, 'x'), temp_src(T0 if dy else T6, 'y')))
        s.append(ins(OP['AND'], temp_dst(T2, 'z'), temp_src(T2, 'z'), temp_src(T2, 'y')))
        s.append(ins(OP['MAD'], temp_dst(T4, 'x'), temp_src(T2, 'z'), temp_src(T2, 'x'), temp_src(T4, 'x')))
        s.append(ins(OP['ADD'], temp_dst(T4, 'y'), temp_src(T4, 'y'), temp_src(T2, 'z')))
    s.append(ins(OP['LT'], temp_dst(T2, 'x'), imm(0.0001), temp_src(T4, 'y')))
    s.append(ins(OP['DIV'], temp_dst(T2, 'y'), temp_src(T4, 'x'), temp_src(T4, 'y')))
    s.append(ins(OP['MOVC'], temp_dst(T2, 'y'), temp_src(T2, 'x'), temp_src(T2, 'y'), imm(1.0)))
    s.append(ins(OP['MUL'], temp_dst(T5, 'x'), temp_src(T5, 'x'), temp_src(T2, 'y')))
    s.append(ins(OP['ENDIF']))
    s.append(ins(OP['ENDIF']))
    return s

# ---- reading the disassembly
def _operand(text):
    m = re.fullmatch(r'r(\d+)\.([xyzw]+)', text.strip())
    if not m:
        raise ValueError('not a temp: %s' % text)
    return int(m.group(1)), m.group(2)

def _cb_to_temp(tokens, reg, swizzle):
    """A mad's src0 cb2[71] -> temp reg.swizzle; the opcode's length fixed."""
    head, ops = split_instruction(tokens)
    op = ops[1]
    if (op[0] >> 12) & 0xFF != 8 or op[-2:] != [CB, TEXEL] or (op[0] >> 31):
        raise ValueError('mad src0 is not cb2[71]')
    ops[1] = temp_src(reg, swizzle)
    return join_instruction(head, ops)

def depth_of(lines):
    d, out = 0, []
    for l in lines:
        b = l.split(' ')[0].split('(')[0]
        if b in ('endif', 'endloop', 'endswitch', 'else', 'case', 'default'):
            d -= 1
        out.append(d)
        if b.startswith(('if_', 'loop', 'switch', 'else', 'case', 'default')):
            d += 1
    return out

def writes(line):
    """[(reg, comps)] a line writes (its destination temps)."""
    name, ops = parse(line)
    base = name.split('(')[0]
    if not ops or base.startswith(('if_', 'breakc', 'retc', 'discard', 'store', 'continuec', 'switch', 'case')):
        return []
    n = 2 if base in ('imul', 'umul', 'udiv', 'swapc', 'sincos', 'uaddc', 'usubb') else 1
    out = []
    for o in ops[:n]:
        m = re.match(r'^r(\d+)\.([xyzw]+)$', o.strip())
        if m:
            out.append((int(m.group(1)), m.group(2)))
    return out

CB12_INDEX = re.compile(r'cb12\[r(\d+)\.([xyzw]) \+ (\d+)\]')
SUM = re.compile(r'mad r(\d+)\.([xyzw]+), (r\d+\.[xyzw]+), (r\d+\.[xyzw]+|cb12\[r\d+\.[xyzw] \+ 1\]\.[xyzw]+), r(\d+)\.([xyzw]+)')

def colour_source(lines, i, reg, comps):
    """The nearest line before i writing reg's colour (comps): a t54 load from +64, or a mov / mul from cb12[r + 1];
    modulation muls of reg by something else passed over. -> (line, ('t54', None) or ('cb12', (reg, comp)))"""
    for k in range(i - 1, -1, -1):
        if not any(r == reg and set(c) & set(comps) for r, c in writes(lines[k])):
            continue
        l = lines[k]
        if re.fullmatch(r'ld_structured_indexable\(structured_buffer, stride=384\)\(mixed,mixed,mixed,mixed\) r%d\.[xyzw]+, r\d+\.[xyzw], l\(64\), t54\.[xyzw]{4}' % reg, l):
            return k, ('t54', None)
        m = CB12_INDEX.search(l)
        if l.startswith(('mov ', 'mul ')) and m and m.group(3) == '1':
            return k, ('cb12', (int(m.group(1)), m.group(2)))
        if l.startswith('mul r%d.' % reg) and ('r%d.' % reg) in l.split(',', 1)[1]:
            continue   # (the colour times a modulation map)
        raise ValueError('the colour r%d comes from: %s' % (reg, l))
    raise ValueError('no colour for r%d' % reg)

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

    def one(rx, what):
        v = find(rx)
        if len(v) != 1:
            raise ValueError('%s found %d times' % (what, len(v)))
        return v[0]
    i_temps = one(r'dcl_temps \d+', 'dcl_temps')
    # ---- the prologue: P in r1.xyz, the view normal in r0
    i_depth = one(r'ld(ms)?_indexable\(texture2d(ms)?\)\(float,float,float,float\) r1\.x, r0\.xyww, t1\.xyzw(, l\(0\)|, v1\.x)?', 'the depth load')
    i_z = one(r'div r1\.z, l\(1\.000000, 1\.000000, 1\.000000, 1\.000000\), r1\.x', 'view z')
    i_px = one(r'mad r1\.x, -r2\.w, cb2\[85\]\.z, r1\.w', 'view x')
    i_n = one(r'mad r0\.(xyz|yzw), r\d+\.[xyzw]{4}, cb2\[2\]\.[xyzw]{4}, r\d+\.[xyzw]{4}', 'view normal')
    ncomp = re.fullmatch(r'mad r0\.(xyz|yzw),.*', lines[i_n]).group(1)
    normal = ncomp + ncomp[0]
    if not (lines[i_depth + 1] == 'mad r1.x, r1.x, cb2[20].x, cb2[20].y' and i_depth < i_z < i_px and i_n < i_px):
        raise ValueError('the prologue differs')
    if not any(re.fullmatch(r'mad r\d+\.yz, v0\.xxyx, cb2\[45\]\.zzwz, cb2\[45\]\.xxyx', l) for l in lines[:i_px]) or \
       not any(re.fullmatch(r'mad r\d+\.xy, r\d+\.yzyy, cb2\[18\]\.xyxx, cb2\[18\]\.zwzz', l) for l in lines[:i_px]):
        raise ValueError('the screen mapping differs')
    for i in range(i_px + 1, len(lines)):
        for reg, comps in writes(lines[i]):
            if (reg == 1 and set(comps) & set('xyz')) or (reg == 0 and set(comps) & set(ncomp)):
                raise ValueError('r%d.%s written after the prologue: %s' % (reg, comps, lines[i]))
    first_code = next(i for i in range(i_temps + 1, len(lines)) if not lines[i].startswith('dcl_'))
    temps = instrs[i_temps][1]
    T = list(range(temps, temps + 7))
    TS = temps + 7
    TV = temps + 8   # (0.9.537) x: the pixel's lowest contact visibility; y: the debug view's switch
    TR = temps + 9   # (0.9.561) x: the ray-traced lamp shadows' switch
    # (0.9.561) the probe's mask (texture2d, like t40: t1 is a texture2dms in the MSAA variants) and table; (0.9.562)
    # the table a texture2d too (4 x 1 texels; was a buffer like t55): both read the same way, as textures
    i_t1 = one(r'dcl_resource_texture2d \(float,float,float,float\) t40', 't40\'s declaration')
    if any(re.fullmatch(r'dcl_resource\w* .* t4[56](, .*)?', l) for l in lines):
        raise ValueError('t45 / t46 already declared')
    dcl_mask = list(instrs[i_t1])
    dcl_table = list(instrs[i_t1])
    if dcl_mask[2] != 40 or len(dcl_mask) != 4:
        raise ValueError('the declarations are not as expected')
    dcl_mask[2] = RT_MASK
    dcl_table[2] = RT_TABLE
    depth = depth_of(lines)
    depth_load = ld_depth(instrs[i_depth], T[6])
    # ---- the lamps' shadow block
    b0 = one(r'mad r\d+\.xyzw, cb2\[71\]\.zwzw, l\(-0\.500000, 0\.500000, 0\.500000, 0\.500000\), r\d+\.zwzw', 'the lamp block')
    b1 = next(i for i in range(b0, len(lines)) if re.fullmatch(r'mul r\d+\.[xyzw], r\d+\.[xyzw], l\(0\.250000\)', lines[i]))
    block = list(range(b0, b1 + 1))
    taps = [i for i in block if lines[i].startswith('sample_c_lz_indexable(texture2d)')]
    mads = [i for i in block if lines[i].startswith('mad ')]
    if b1 - b0 != 9 or len(taps) != 4 or not all(', t43.xxxx, s14, ' in lines[i] for i in taps) or mads != [b0, b0 + 4]:
        raise ValueError('the lamp block is not as expected')
    _, ops0 = parse(lines[b0])
    a_reg, a_swz = _operand(ops0[3])
    _, ops4 = parse(lines[b0 + 4])
    if _operand(ops4[0]) != (a_reg, 'xyzw') or _operand(ops4[3]) != (a_reg, 'xyzw') or a_swz != 'zwzw':
        raise ValueError('the lamp position register differs')
    _, opsm = parse(lines[b1])
    d_reg, d_comp = _operand(opsm[0])
    if {parse(lines[i])[1][4].strip() for i in taps} != {'r%d.%s' % (d_reg, d_comp)}:
        raise ValueError('the lamp taps compare with another register')
    needed, _ = live_in([lines[i] for i in block])
    if needed - ({(a_reg, c) for c in 'xyzw'} | {(d_reg, d_comp)}):
        raise ValueError('the lamp block reads more')
    K, C, D = T[0], T[1], T[2]   # (dead outside the block and the traces)
    lamp_body = [_cb_to_temp(instrs[i], K, 'zwzw') if i in mads else instrs[i] for i in block]
    lamp = [ins(OP['MUL'], temp_dst(K, 'xy'), cb_src(CB, TEXEL, 'zwzz'), temp_src(TS, 'wwww')),
            ins(OP['MUL'], temp_dst(K, 'zw'), cb_src(CB, TEXEL, 'zzzw'), temp_src(TS, 'zzzz')),
            ins(OP['MOV'], temp_dst(C, 'xyzw'), temp_src(a_reg, 'zwzw')),
            ins(OP['MOV'], temp_dst(D, 'x'), temp_src(d_reg, d_comp)),
            ins(OP['MOV'], temp_dst(D, 'y'), imm(0.0))]
    for dx, dy in ((-1.0, -1.0), (1.0, -1.0), (-1.0, 1.0), (1.0, 1.0)):
        lamp.append(ins(OP['MAD'], temp_dst(a_reg, 'xyzw'), temp_src(K, 'xyxy'), imm(dx, dy, dx, dy), temp_src(C, 'xyzw')))
        lamp.append(ins(OP['MOV'], temp_dst(d_reg, d_comp), temp_src(D, 'x')))
        lamp.extend(lamp_body)
        lamp.append(ins(OP['ADD'], temp_dst(D, 'y'), temp_src(D, 'y'), temp_src(d_reg, d_comp)))
    lamp.append(ins(OP['MUL'], temp_dst(d_reg, d_comp), temp_src(D, 'y'), imm(0.25)))
    # ---- the lights' sums
    sums = []
    for i, l in enumerate(lines):
        m = SUM.fullmatch(l)
        if m and m.group(1) == m.group(5):
            sums.append(i)
    groups = []
    for i in sums:
        if groups and i - groups[-1][-1] <= 2:
            groups[-1].append(i)
        else:
            groups.append([i])
    groups = [g for g in groups if len(g) >= 2]
    if len(groups) != 4:
        raise ValueError('%d light sums: %s' % (len(groups), [lines[g[0]] for g in groups]))
    after, before, sources = {}, {}, []
    for n, g in enumerate(groups):
        colour = SUM.fullmatch(lines[g[0]]).group(4)
        m = CB12_INDEX.match(colour)
        if m:
            src_line, source = g[0], ('cb12', (int(m.group(1)), m.group(2)))
        else:
            creg, cswz = _operand(colour)
            src_line, source = colour_source(lines, g[0], creg, set(cswz))
        for i in g[1:]:
            c = SUM.fullmatch(lines[i]).group(4)
            if (c.startswith('cb12') and not m) or (not c.startswith('cb12') and c.split('.')[0] != colour.split('.')[0]):
                raise ValueError('light %d: its sums read other colours' % n)
        if source[0] == 't54':
            _, lops = parse(lines[src_line])
            ireg, icomp = _operand(lops[1])
            source = ('t54', instrs[src_line])
        else:
            ireg, icomp = source[1]
        # where the light's index still holds and the trace runs whenever the sums do: right before the sums; else
        # right after its colour (not in a branch the sums are outside of); else right after the index is made
        kept = lambda a, b: not any(r == ireg and icomp in c for k in range(a, b) for r, c in writes(lines[k]))

        def encloses(k):
            """Code put after line k runs whenever the sums do: no block it is in closes before them."""
            opens = lines[k].split(' ')[0].split('(')[0].startswith(('if_', 'loop', 'else', 'switch', 'case', 'default'))
            inner = depth[k] + (1 if opens else 0)
            return min(depth[j] for j in range(k + 1, g[0] + 1)) >= inner
        at = None
        if src_line == g[0] or kept(src_line + 1, g[0]):
            at = g[0]
        elif encloses(src_line):
            at = src_line + 1
        else:
            made = next((k for k in range(src_line - 1, -1, -1)
                         if any(r == ireg and icomp in c for r, c in writes(lines[k]))), None)
            if made is not None and kept(made + 1, src_line) and encloses(made):
                at = made + 1
        if at is None:
            raise ValueError('light %d: no place for its trace' % n)
        place = at - 1 if at != g[0] else None
        # the sums' factors, times the visibility (copies in T5.y / T5.z)
        factors = []
        for i in g:
            f = _operand(SUM.fullmatch(lines[i]).group(3))
            f = (f[0], f[1][0] if len(set(f[1])) == 1 else None)
            if f[1] is None:
                raise ValueError('light %d: a factor not a scalar' % n)
            if f not in factors:
                factors.append(f)
        if len(factors) > 2:
            raise ValueError('light %d: %d factors' % (n, len(factors)))
        pre = []
        for k, (reg, comp) in enumerate(factors):
            pre.append(ins(OP['MUL'], temp_dst(T[5], 'yz'[k]), temp_src(reg, comp), temp_src(T[5], 'x')))
        new_sums = {}
        for i in g:
            f = _operand(SUM.fullmatch(lines[i]).group(3))
            k = factors.index((f[0], f[1][0]))
            head, ops = split_instruction(instrs[i])
            ops[1] = temp_src(T[5], 'yz'[k] * 4)
            new_sums[i] = join_instruction(head, ops)
        trace = contact_trace(source, depth_load, normal, T, TS, TV)
        trace.extend(rt_visibility(source, T, TR))   # (0.9.561)
        if place is None:
            before.setdefault(g[0], []).extend(trace)
        else:
            after.setdefault(place, []).extend(trace)
        before.setdefault(g[0], []).extend(pre)
        for i, t in new_sums.items():
            instrs[i] = t
        sources.append('%s@%d->%d' % (source[0], src_line, at))
    out = []
    for i, tok in enumerate(instrs):
        if i == i_temps:
            out.append(dcl_mask)   # (0.9.561)
            out.append(dcl_table)
            out.append([tok[0], temps + 10])
            continue
        if i == first_code:
            out.append(ins(OP['MOV'], temp_dst(TS, 'xyzw'), imm(M_STRENGTH, M_LENGTH, M_INNER, M_CENTRE)))
            out.append(ins(OP['MOV'], temp_dst(TV, 'xy'), imm(1.0, M_DEBUG, 0.0, 0.0)))
            out.append(ins(OP['MOV'], temp_dst(TR, 'x'), imm(M_RT)))   # (0.9.561)
        if i in before:
            out.extend(before[i])
        if lines[i] == 'ret':   # (0.9.537) the debug view: the light white, the contact shadows red
            out.append(ins(OP['LT'], temp_dst(TV, 'z'), imm(0.5), temp_src(TV, 'y')))
            out.append(ins(OP['IF'], temp_src(TV, 'z'), nz=True))
            out.append(ins(OP['MOV'], output_dst(0, 'x'), imm(1.0)))
            out.append(ins(OP['MOV'], output_dst(0, 'yz'), temp_src(TV, 'xxxx')))
            out.append(ins(OP['MOV'], output_dst(1, 'xyz'), imm(0.0, 0.0, 0.0, 0.0)))
            out.append(ins(OP['ENDIF']))
        if i == b0:
            out.extend(lamp)
        if b0 <= i <= b1:
            continue
        out.append(tok)
        if i in after:
            out.extend(after[i])
    new_code = dxbc.code_bytes(version, out)
    patched = dxbc.build([(t, new_code if t == code_tag else d) for t, d in chunk_list])
    offsets = {}
    if sum(1 for l in lines if l == 'ret') != 2:
        raise ValueError('%d returns' % sum(1 for l in lines if l == 'ret'))
    for name, value in (('strength', M_STRENGTH), ('length', M_LENGTH), ('inner', M_INNER), ('centre', M_CENTRE),
                        ('debug', M_DEBUG), ('rt', M_RT)):
        marker = struct.pack('<f', value)
        if patched.count(marker) != 1:
            raise ValueError('the %s marker is found %d times' % (name, patched.count(marker)))
        offsets[name] = patched.index(marker)
    return patched, dict(offsets=offsets, temps=(temps, temps + 10), lights=sources)

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name in sys.argv[3:]:
        blob = open(os.path.join(src, name + '.dxbc'), 'rb').read()
        try:
            patched, info = patch(blob)
            open(os.path.join(out, name + '.dxbc'), 'wb').write(patched)
            open(os.path.join(out, name + '.asm'), 'w').write(dxbc.disassemble(patched).replace('\x00', ''))
            print(name, 'ok', len(blob), '->', len(patched), info['temps'], info['lights'])
        except Exception as e:
            print(name, 'FAILED', e)
