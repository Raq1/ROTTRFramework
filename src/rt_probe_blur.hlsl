// (v0.9.564) The lamp shadow mask's blur: each texel's three lamp visibilities averaged over the texels around it
// (Radius each way, tent weights) whose view depth agrees with its own (|depth - d| <= 0.03 d + 3 cm, as the lights'
// pass tests), so the soft shadows' ray noise is smoothed without bleeding across edges. The depth (.w) is kept as it is;
// a texel that hit nothing (.w 0), or Radius 0, is copied. Raw: rt_probe_shadow.hlsl's output; Blurred: what the lights'
// pass and the probe's view read. Compiled to DXIL (cs_6_0) into rt_probe_blur_cs.inc by tools/rt_probe/build_shader.bat.
Texture2D<float4> Raw : register(t0);
RWTexture2D<float4> Blurred : register(u0);

cbuffer Blur : register(b0) {
    uint Width;
    uint Height;
    int Radius;
    uint Spare;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    const float4 centre = Raw[id.xy];
    if (Radius <= 0 || centre.w <= 0.0) {
        Blurred[id.xy] = centre;
        return;
    }
    const float tolerance = 0.03 * centre.w + 3.0;
    float3 sum = 0.0;
    float weight = 0.0;
    for (int y = -Radius; y <= Radius; ++y) {
        for (int x = -Radius; x <= Radius; ++x) {
            const int2 at = int2(id.xy) + int2(x, y);
            if (at.x < 0 || at.y < 0 || at.x >= (int)Width || at.y >= (int)Height) continue;
            const float4 s = Raw[at];
            if (abs(s.w - centre.w) > tolerance) continue;
            const float w = (Radius + 1 - abs(x)) * (Radius + 1 - abs(y));
            sum += s.xyz * w;
            weight += w;
        }
    }
    Blurred[id.xy] = float4(sum / weight, centre.w);
}
