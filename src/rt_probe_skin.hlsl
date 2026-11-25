// (v0.9.545) The ray tracing probe's skinning (rt_probe_gpu.cpp): a skinned mesh posed as the game's skinned vertex
// shaders pose it (a vs_5_0 of the game's shader libraries, cb1 SkinningBuffer): the palette entry of each of the four
// blend indices (BLENDINDICES x 255, rounded) is three registers (its matrix's columns: world = dot(register,
// (position, 1)), absolute world space), the four blend weights used as stored; summed. One thread per vertex; the
// positions written as float3. Compiled to DXIL (cs_6_0) into rt_probe_skin_cs.inc by tools/rt_probe/build_shader.bat.
ByteAddressBuffer Positions : register(t0);
ByteAddressBuffer Weights : register(t1);
ByteAddressBuffer Indices : register(t2);
ByteAddressBuffer Palette : register(t3);   // the game's: 64 bytes an entry (three matrix registers, one of effects)
RWByteAddressBuffer Output : register(u0);

cbuffer Job : register(b0) {
    uint FirstVertex;
    uint VertexCount;
    uint PositionStride;
    uint PositionOffset;
    uint WeightStride;
    uint WeightOffset;
    uint WeightFormat;   // 0 R8G8B8A8_UNORM, 1 R16G16B16A16_UNORM, 2 R16G16B16A16_FLOAT, 3 R32G32B32A32_FLOAT
    uint IndexStride;
    uint IndexOffset;
    uint IndexFormat;
    uint PaletteCount;
    uint Use;   // (v0.9.547) bits 0..3 the weight components read, 4..7 the index components (no weights: weight 1 on the first)
};

float4 Decode(ByteAddressBuffer buffer, uint address, uint format) {
    if (format == 0) {
        const uint v = buffer.Load(address);
        return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
    }
    if (format == 1) {
        const uint2 v = buffer.Load2(address);
        return float4(v.x & 0xFFFF, v.x >> 16, v.y & 0xFFFF, v.y >> 16) / 65535.0;
    }
    if (format == 2) {
        const uint2 v = buffer.Load2(address);
        return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16));
    }
    return asfloat(buffer.Load4(address));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= VertexCount) return;
    const uint v = FirstVertex + id.x;
    const float4 p = float4(asfloat(Positions.Load3(v * PositionStride + PositionOffset)), 1.0);
    const uint weightUse = Use & 0xF, indexUse = (Use >> 4) & 0xF;
    const float4 w = weightUse ? Decode(Weights, v * WeightStride + WeightOffset, WeightFormat) : float4(1, 0, 0, 0);
    const uint4 bone = (uint4)round(Decode(Indices, v * IndexStride + IndexOffset, IndexFormat) * 255.0);
    float3 posed = 0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        if (!(indexUse & (1u << k)) || (weightUse && !(weightUse & (1u << k))) || w[k] == 0.0) continue;
        const uint entry = min(bone[k], PaletteCount - 1) * 64;
        const float4 c0 = asfloat(Palette.Load4(entry));
        const float4 c1 = asfloat(Palette.Load4(entry + 16));
        const float4 c2 = asfloat(Palette.Load4(entry + 32));
        posed += w[k] * float3(dot(c0, p), dot(c1, p), dot(c2, p));
    }
    Output.Store3(id.x * 12, asuint(posed));
}
