// (v0.9.560) The ray tracing probe's lamp shadow mask: for each texel (half the game's render resolution) the ray of
// the game's own pixel mapping (SceneBuffer: ScreenExtents, the projection's scale and offset, View) through the probe's
// scene; from its hit, a shadow ray to each of up to three lamps whose reach it is in. Out: visibility per lamp (1 lit,
// 0 shadowed, 1 out of its reach) and the hit's view depth (0: nothing hit), and a table of the lamps traced (their
// view-space positions as the lights' pass has them, for matching there). Alpha-tested meshes are tested as the view
// does. Compiled to DXIL (cs_6_5) into rt_probe_shadow_cs.inc by tools/rt_probe/build_shader.bat.
// (v0.9.564) Soft shadows: a lamp is given a size (its radius: 2 % of its reach, 4 .. 30 cm, times Softness) and
// RayCount rays go to points spread over a disc of that radius facing the texel (a Vogel spiral, turned per texel by
// interleaved gradient noise and per lamp); the visibility is the share of them that get through. The noise is the same
// every frame (no flicker); rt_probe_blur.hlsl smooths it.
// (v0.9.570) Mode 1, the sun: a texel's surface traced up to SunDistance; from it, rays toward the sun in a cone of its
// disc (two first, the others only at an edge), 1 km long; Mask .x the sun's visibility (.yz 1, .w the depth as before),
// the Table left alone. The sun's shadow pass reads it (patch.py) where its depth agrees, the cascades elsewhere.
// The instances' masks: 1 certain, 2 uncertain (an alpha test not resolved: traced as solid, so possibly a leaf card's
// whole quad). The lamps' and the first rays take both; a sun ray takes the certain ones, and where it is not blocked but
// goes through an uncertain one the texel is given up (depth 0: the cascades there).
// (v0.9.571) Cheaper: the uncertain instances are not opaque (their record kUncertain): the lamps' and the first rays
// take them as solid (CutoutPasses), the sun's rays note them in the same traversal (SunRay; was a second one). The masks
// are at most 1280 wide (the render size over PixelScale, PixelScaleY: any ratio). The sun's surfaces traced to 150 m
// (was 250).
// (v0.9.565) Cheaper: a texel's first ray is traced only along the stretch inside some lamp's reach (from the camera to
// the furthest exit from a reach sphere; a ray that crosses none is not traced: the texel lit, its depth 0 so the
// lights' pass and the blur leave it out). The soft rays go to points on a ring at 0.71 of the lamp's radius, two
// opposite ones first; only where those two disagree (a shadow's edge) are the others cast.
RaytracingAccelerationStructure Scene : register(t0);

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

RWTexture2D<float4> Mask : register(u0);
RWTexture2D<float4> Table : register(u1);   // (v0.9.562: 4 x 1 texels; was a buffer)

cbuffer Shadow : register(b0) {
    float4 View[4];       // SceneBuffer's View (rows: view = world x View)
    float4 Projection;    // SceneBuffer[18]: view x / z = uv.x * x + z, view y / z = uv.y * y + w
    float4 Extents;       // SceneBuffer[45] (ScreenExtents): uv = pixel x zw + xy (the game's render pixels)
    float4 Lamp[3];       // world position, reach
    float4 LampView[3];   // the lights' pass's view-space positions (cb12 +0), for the table
    uint LampCount;
    uint MaskWidth;
    uint MaskHeight;
    float PixelScale;     // render pixels a mask texel across (2; v0.9.571: the render width over the mask's)
    float Softness;       // (v0.9.564) the lamps' size: 0 one ray (sharp), 1 the size from the reach, up to 3
    uint RayCount;        // (v0.9.564) rays a lamp when soft
    float2 Spare;
    float4 Sun;           // (v0.9.570) xyz: toward the sun (world, unit); w: the tangent of its disc's angular radius
    float SunDistance;    // (v0.9.570) the farthest a texel's surface is traced for the sun (cm from the camera)
    uint Mode;            // (v0.9.570) 0: the lamps (Mask .xyz, Table); 1: the sun (Mask .x; .yz 1; no Table)
    float PixelScaleY;    // (v0.9.571) and down (the render height over the mask's)
    float Spare2;
};

static const uint kUncertain = 0xFFFFFF;   // (v0.9.571) an uncertain instance's record: solid here, noted by the sun
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

// the nearest hit's distance (-1: none)
float TraceNearest(float3 origin, float3 direction, float tMin, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            CutoutPasses(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidatePrimitiveIndex(),
                         q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
}

// whether anything lies between (the first hit ends it); (v0.9.570) among the instances of mask
bool Blocked(float3 origin, float3 direction, float tMin, float tMax, uint mask) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            CutoutPasses(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidatePrimitiveIndex(),
                         q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// (v0.9.565) how far along a ray from the camera the stretch inside some lamp's reach goes (0: it crosses none)
float ReachEnd(float3 origin, float3 dir) {
    float end = 0.0;
    [unroll] for (uint k = 0; k < 3; ++k) {
        if (k >= LampCount) break;
        const float3 oc = origin - Lamp[k].xyz;
        const float b = dot(oc, dir);
        const float h = b * b - (dot(oc, oc) - Lamp[k].w * Lamp[k].w);
        if (h > 0.0) end = max(end, -b + sqrt(h));
    }
    return end;
}

// (v0.9.570) the sun's visibility from a point: rays in a cone around the way to the sun (its disc's angular radius:
// spread), on a ring at 0.71 of it, turned per texel; two opposite ones first, the others only where they disagree
static const float kSunRay = 100000.0;   // (1 km: casters farther toward the sun are not traced)
// (-1: given up, a ray not blocked went through an uncertain instance); (v0.9.571) one traversal: an uncertain
// candidate noted and passed by, the first other hit ends it
float SunRay(float3 from, float3 dir) {
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = dir;
    ray.TMin = 0.5;
    ray.TMax = kSunRay;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    bool uncertain = false;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        const uint record = q.CandidateInstanceContributionToHitGroupIndex();
        if (record == kUncertain) {
            uncertain = true;
            continue;
        }
        if (CutoutPasses(record, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return 0.0;
    return uncertain ? -1.0 : 1.0;
}
float SunVisibility(float3 from, float3 toSun, float spread, uint rays, float turn) {
    if (rays <= 1 || spread <= 0.0) return SunRay(from, toSun);
    const float3 a = abs(toSun.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(a, toSun));
    const float3 b = cross(toSun, t);
    const float r = spread * 0.70710678;
    uint open = 0;
    for (uint i = 0; i < rays; ++i) {
        const uint j = i == 0 ? 0 : i == 1 ? rays / 2 : (i - 1 < rays / 2 ? i - 1 : i);
        const float angle = turn + 6.2831853 * j / rays;
        const float3 dir = normalize(toSun + (t * cos(angle) + b * sin(angle)) * r);
        const float v = SunRay(from, dir);
        if (v < 0.0) return -1.0;
        if (v > 0.0) ++open;
        if (i == 1 && open != 1) return open * 0.5;
    }
    return open / (float)rays;
}

// (v0.9.564) a lamp's visibility from a point: one ray to its centre, or rays to points over its disc
float LampVisibility(float3 from, float4 lamp, float radius, uint rays, float turn) {
    const float3 toLamp = lamp.xyz - from;
    const float d = length(toLamp);
    if (d >= lamp.w || d < 1.0) return 1.0;
    if (rays <= 1 || radius <= 0.0) {
        // stopped short of the lamp (its own fixture, a lantern's glass, is not a shadow)
        return Blocked(from, toLamp / d, 0.5, d - min(25.0, 0.5 * d), 0xFF) ? 0.0 : 1.0;
    }
    const float3 n = toLamp / d;
    const float3 a = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(a, n));
    const float3 b = cross(n, t);
    // (v0.9.565) points on a ring at 0.71 of the radius, turned per texel; in the order 0, rays / 2 (opposite), then the
    // rest: when the first two agree the texel is taken as wholly lit or wholly shadowed
    const float r = radius * 0.70710678;
    uint open = 0;
    for (uint i = 0; i < rays; ++i) {
        const uint j = i == 0 ? 0 : i == 1 ? rays / 2 : (i - 1 < rays / 2 ? i - 1 : i);
        const float angle = turn + 6.2831853 * j / rays;
        const float3 to = lamp.xyz + (t * cos(angle) + b * sin(angle)) * r - from;
        const float dt = length(to);
        if (!Blocked(from, to / dt, 0.5, dt - min(25.0, 0.5 * dt), 0xFF)) ++open;
        if (i == 1 && open != 1) return open * 0.5;
    }
    return open / (float)rays;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (Mode == 0 && id.x == 0 && id.y == 0)   // (v0.9.570: the lamps' only)
        for (uint k = 0; k < 3; ++k) Table[uint2(k, 0)] = float4(LampView[k].xyz, k < LampCount ? 1.0 : 0.0);
    if (id.x >= MaskWidth || id.y >= MaskHeight) return;
    // the render pixel at the texel's centre, as the lights' pass maps its pixels
    const float2 pixel = (float2(id.xy) + 0.5) * float2(PixelScale, PixelScaleY);   // (v0.9.571: per axis)
    const float2 uv = pixel * Extents.zw + Extents.xy;
    const float3 dv = float3(uv.x * Projection.x + Projection.z, uv.y * Projection.y + Projection.w, 1.0);
    // to the world: world = (view - T) x R^T, R = View's rows' xyz, T = its fourth row
    const float3 dir = normalize(float3(dot(dv, View[0].xyz), dot(dv, View[1].xyz), dot(dv, View[2].xyz)));
    const float3 t3 = View[3].xyz;
    const float3 camera = -float3(dot(t3, View[0].xyz), dot(t3, View[1].xyz), dot(t3, View[2].xyz));
    float4 result = float4(1.0, 1.0, 1.0, 0.0);
    const float end = Mode == 1 ? SunDistance : ReachEnd(camera, dir);   // (v0.9.565; was 200,000; v0.9.570 the sun's)
    if (end <= 1.0) {
        Mask[id.xy] = result;
        return;
    }
    const float t = TraceNearest(camera, dir, 1.0, end);
    if (t > 0.0) {
        const float3 hit = camera + dir * t;
        result.w = t / length(dv);   // the view depth (P = z x dv)
        // pulled back towards the camera a little: the surface itself is not hit again
        const float3 from = hit - dir * (0.5 + 0.0005 * t);
        // (v0.9.564) the texel's turn of the ray pattern: interleaved gradient noise (Jimenez), the same each frame
        const float noise = frac(52.9829189 * frac(dot(float2(id.xy), float2(0.06711056, 0.00583715))));
        if (Mode == 1) {   // (v0.9.570)
            const float sun = SunVisibility(from, Sun.xyz, Sun.w, RayCount, 6.2831853 * noise);
            if (sun < 0.0)
                result.w = 0.0;   // given up: the cascades here
            else
                result.x = sun;
        } else {
            [unroll] for (uint k = 0; k < 3; ++k) {
                if (k >= LampCount) break;
                const float radius = clamp(0.02 * Lamp[k].w, 4.0, 30.0) * Softness;
                result[k] = LampVisibility(from, Lamp[k], radius, RayCount, 6.2831853 * noise + 1.7 * k);
            }
        }
    }
    Mask[id.xy] = result;
}
