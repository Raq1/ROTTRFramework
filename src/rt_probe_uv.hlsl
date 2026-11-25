// (v0.9.552) The ray tracing probe's leaf cutouts: a mesh's UVs gathered once, three a triangle in the order of its
// index range (the acceleration structure's primitive order), from the game's own index buffer and UV stream (root
// views: 4-byte aligned starts, the rest as skews). Stored as decoded but unscaled; the view shader applies each
// draw's scale and offset. Compiled to DXIL into rt_probe_uv_cs.inc by tools/rt_probe/build_shader.bat.
cbuffer Job : register(b0) {
    uint Triangles;
    uint IndexSize;   // 2 or 4
    uint IndexSkew;   // the first index's bytes past the aligned start
    uint UvStride;
    uint UvKind;      // 0 float32, 1 float16, 2 snorm16, 3 unorm16, 4 snorm8, 5 unorm8
    uint UvAtU;       // u's bytes into a vertex (the start's skew included)
    uint UvAtV;
    uint OutAt;       // the first triangle's first UV (float2 index into the arena)
};
ByteAddressBuffer Indices : register(t0);
ByteAddressBuffer Uvs : register(t1);
RWByteAddressBuffer Arena : register(u0);

uint LoadIndex(uint i) {
    const uint at = IndexSkew + i * IndexSize;
    const uint word = Indices.Load(at & ~3u);
    return IndexSize == 4 ? word : ((at & 2u) != 0 ? (word >> 16) : (word & 0xFFFFu));
}

float LoadComponent(uint at) {
    const uint word = Uvs.Load(at & ~3u);
    if (UvKind == 0) return asfloat(word);
    const uint shift = (at & 3u) * 8u;
    if (UvKind <= 3) {
        const uint h = (word >> shift) & 0xFFFFu;
        if (UvKind == 1) return f16tof32(h);
        if (UvKind == 2) return max(float(asint(h << 16) >> 16) / 32767.0, -1.0);
        return float(h) / 65535.0;
    }
    const uint b = (word >> shift) & 0xFFu;
    if (UvKind == 4) return max(float(asint(b << 24) >> 24) / 127.0, -1.0);
    return float(b) / 255.0;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Triangles) return;
    for (uint k = 0; k < 3; ++k) {
        const uint v = LoadIndex(id.x * 3 + k);
        const uint at = v * UvStride;
        const float2 uv = float2(LoadComponent(at + UvAtU), LoadComponent(at + UvAtV));
        Arena.Store2((OutAt + id.x * 3 + k) * 8, asuint(uv));
    }
}
