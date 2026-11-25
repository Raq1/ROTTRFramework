"""The hair resolve shader's (ROTTR.exe 0x137C4AC) shadow-range bias: `add rX, rX, l(1.0)` after the occluder depth's
linearisation (`div rX, rK, rX`, its sample of t43 three instructions before), in its four shadow paths. Prints the
blob's checksum and the byte offsets of those four immediates."""
import sys, struct, re, os
sys.path.insert(0, os.path.dirname(__file__))
import dxbc
blob = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<I', blob, 28)[0]
offs = [struct.unpack_from('<I', blob, 32 + 4 * i)[0] for i in range(count)]
shex_at = next(o for o in offs if blob[o:o + 4] in (b'SHEX', b'SHDR')) + 8
version, ins = dxbc.instructions(dict(dxbc.chunks(blob))[b'SHEX'])
lines = dxbc.instruction_lines(dxbc.disassemble(blob))
assert len(lines) == len(ins)
pos = shex_at + 8
found = []
for i, (tok, line) in enumerate(zip(ins, lines)):
    m = re.fullmatch(r'add (r\d+)\.([xyzw]), \1\.\2, l\(1\.000000\)', line)
    reg = m and m.group(1) + r'\.' + m.group(2)
    if m and re.fullmatch(r'div ' + reg + r', r\d+\.[xyzw], ' + reg, lines[i - 1]) and ', t43.' in lines[i - 3]:
        at = pos + 4 * (len(tok) - 1)
        assert struct.unpack_from('<I', blob, at)[0] == 0x3F800000
        found.append(at)
    pos += 4 * len(tok)
print('checksum', ', '.join('0x%08Xu' % w for w in struct.unpack('<4I', blob[4:20])), 'size', len(blob))
print('offsets', ', '.join('0x%X' % f for f in found))
