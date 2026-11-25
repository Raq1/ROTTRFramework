// (v0.9.543) The ray tracing probe's debug view (rt_probe.cpp): one camera ray per pixel through the acceleration
// structure built from the game's own draws. A clay render: each object its own hue, lit by a light near the camera
// from normals rebuilt from the neighbouring pixels' hits (the vertices are not read); a miss is a sky gradient.
// Compiled to DXIL (cs_6_5: inline ray queries) into rt_probe_cs.inc by tools/rt_probe/build_shader.bat.
// (v0.9.552) Leaf cutouts: an alpha-tested mesh's geometry is not opaque; a candidate hit on it is taken when its texel
// passes the game's test (the instance's InstanceContributionToHitGroupIndex: its cutout record + 1; 0: taken as is).
// The record: the draw's UV scale and offset, its texture (an index into this frame's table), the component compared,
// the threshold, and where the mesh's UVs are (three a triangle, gathered once: rt_probe_uv.hlsl).
RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> Output : register(u0);

struct CutoutRecord {
    float2 Scale;
    float2 Offset;
    uint Texture;
    uint Channel;
    float Threshold;
    uint UvAt;
};
StructuredBuffer<CutoutRecord> Cutouts : register(t1);
ByteAddressBuffer CutoutUvs : register(t2);
Texture2D<float4> Textures[2048] : register(t0, space1);
SamplerState Wrap : register(s0);

cbuffer Camera : register(b0) {
    float3 Origin;
    float TanHalf;   // tan(vertical fov / 2)
    float3 Right;
    float Aspect;    // width / height
    float3 Up;
    float Width;
    float3 Forward;
    float Height;
    float MaxDistance;
    float MinDistance;
    float KindView;   // (v0.9.548) 1: each object coloured by how it was captured (its ID's bits 21..23), not its own hue
    float MaskScale;  // (v0.9.560) > 0: the lamp shadow mask over the view (a view pixel x this: its texel)
};
Texture2D<float4> Mask : register(t3);   // (v0.9.560) rt_probe_shadow.hlsl's: per lamp visibility, the view depth

groupshared float3 g_position[8][8];
groupshared uint g_object[8][8];   // the hit's instance ID + 1 (0: a miss)

// (v0.9.548) 0 static, 1 instanced, 2 a skinned layout drawn with the world matrix, 3 four-weight skinned, 4 one-bone
// skinned, 5 tessellated; (v0.9.550) 6 skinned, an instance of an instanced draw, 7 skinned, drawn only without depth writes
float3 KindColour(uint id) {
    const uint kind = (id >> 21) & 7;
    if (kind == 1) return float3(0.25, 0.75, 0.30);
    if (kind == 2) return float3(0.90, 0.80, 0.20);
    if (kind == 3) return float3(0.90, 0.20, 0.20);
    if (kind == 4) return float3(0.25, 0.40, 0.95);
    if (kind == 5) return float3(0.20, 0.80, 0.85);
    if (kind == 6) return float3(1.00, 0.55, 0.10);
    if (kind == 7) return float3(1.00, 0.55, 0.85);
    return float3(0.60, 0.60, 0.60);
}

// (v0.9.552) a candidate hit on an alpha-tested mesh: its texel against the game's threshold
// (v0.9.571) an uncertain instance's record (rt_probe_shadow.hlsl's kUncertain: an alpha test not resolved, not opaque
// in the top level): drawn as solid here, its record never read (0xFFFFFF is no record: a root view is not bounds-checked)
static const uint kUncertain = 0xFFFFFF;
bool CutoutPasses(uint record, uint primitive, float2 barycentrics) {
    if (record == 0 || record == kUncertain) return true;
    const CutoutRecord c = Cutouts[record - 1];
    const uint at = (c.UvAt + primitive * 3) * 8;
    const float2 a = asfloat(CutoutUvs.Load2(at));
    const float2 b = asfloat(CutoutUvs.Load2(at + 8));
    const float2 d = asfloat(CutoutUvs.Load2(at + 16));
    const float2 uv = (a + (b - a) * barycentrics.x + (d - a) * barycentrics.y) * c.Scale + c.Offset;
    const float4 t = Textures[NonUniformResourceIndex(c.Texture)].SampleLevel(Wrap, uv, 0);
    const float alpha = c.Channel == 0 ? t.x : c.Channel == 1 ? t.y : c.Channel == 2 ? t.z : t.w;
    return alpha >= c.Threshold;
}

float3 Hue(uint id) {
    const uint h = id * 2654435761u;
    return 0.3 + 0.7 * float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID) {
    const float2 ndc = float2((id.x + 0.5) / Width * 2.0 - 1.0, 1.0 - (id.y + 0.5) / Height * 2.0);
    const float3 dir = normalize(Forward + Right * (ndc.x * TanHalf * Aspect) + Up * (ndc.y * TanHalf));
    RayDesc ray;
    ray.Origin = Origin;
    ray.Direction = dir;
    ray.TMin = MinDistance;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_NONE> query;   // (v0.9.552: was RAY_FLAG_FORCE_OPAQUE; the opaque instances say so themselves)
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            CutoutPasses(query.CandidateInstanceContributionToHitGroupIndex(), query.CandidatePrimitiveIndex(),
                         query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float t = hit ? query.CommittedRayT() : MaxDistance;
    const uint object = hit ? query.CommittedInstanceID() + 1 : 0;
    g_position[local.y][local.x] = Origin + dir * t;
    g_object[local.y][local.x] = object;
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= (uint)Width || id.y >= (uint)Height) return;

    const float3 sky = lerp(float3(0.62, 0.64, 0.66), float3(0.30, 0.45, 0.70), saturate(ndc.y * 0.5 + 0.5));
    if (!hit) {
        Output[id.xy] = float4(sky, 1.0);
        return;
    }
    // the surface's normal from the neighbours' hits on the same object (else facing the camera)
    const int x = (int)local.x, y = (int)local.y;
    const int sx = x < 7 ? 1 : -1, sy = y < 7 ? 1 : -1;
    const float3 p = g_position[y][x];
    float3 n = -dir;
    if (g_object[y][x + sx] == object && g_object[y + sy][x] == object) {
        const float3 dx = (g_position[y][x + sx] - p) * sx;
        const float3 dy = (g_position[y + sy][x] - p) * sy;
        const float3 c = cross(dy, dx);
        if (dot(c, c) > 1e-12) n = normalize(c);
    }
    if (dot(n, dir) > 0.0) n = -n;
    const float3 light = normalize(-dir + Up * 0.6 + Right * 0.35);
    const float lambert = saturate(dot(n, light));
    float3 color = (KindView > 0.5 ? KindColour(object - 1) : Hue(object - 1)) * (0.22 + 0.78 * lambert);
    if (MaskScale > 0.0) {   // (v0.9.560) a lamp's shadow takes its channel away (red, green, blue: lamps 1 to 3)
        const float4 m = Mask.Load(int3(int2(float2(id.xy) * MaskScale), 0));
        color *= 0.15 + 0.85 * m.rgb;
    }
    const float fog = saturate(t / MaxDistance);
    color = lerp(color, sky, fog * fog);
    Output[id.xy] = float4(color, 1.0);
}
