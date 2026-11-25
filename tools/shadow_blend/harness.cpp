// Runs a ROTTR shadow-mask pixel shader (the original or the patched) on WARP with test data and prints the result
// per depth: 4 cascades whose maps hold alternating values (cascade 3 and 1 lit, 2 and 0 shadowed), the view depth
// growing down the screen.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static std::vector<char> ReadAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const int W = 64, H = 256, A = 256;   // the screen; the atlas (A x A, four quadrants)
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("no device\n");
        return 3;
    }
    const auto psCode = ReadAll(argv[1]);
    ID3D11PixelShader* ps = nullptr;
    HRESULT hr = dev->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps);
    std::printf("CreatePixelShader 0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 4;
    const char* vsSrc = "float4 main(uint id : SV_VertexID) : SV_POSITION { float2 p = float2((id << 1) & 2, id & 2);"
                        " return float4(p * float2(2, -2) + float2(-1, 1), 0.5, 1); }";
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err))) return 5;
    ID3D11VertexShader* vs = nullptr;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);

    // t1: the depth (1 / view z: cb2[20] = (1, 0) gives z back); z from 50 to 1325 down the screen
    std::vector<float> depth(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) depth[y * W + x] = 1.0f / (50.0f + 5.0f * y);
    auto texture = [&](int w, int h, const float* data, ID3D11ShaderResourceView** srv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_IMMUTABLE, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA s{data, UINT(w * 4), 0};
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, &s, &t);
        dev->CreateShaderResourceView(t, nullptr, srv);
    };
    ID3D11ShaderResourceView* depthSrv = nullptr;
    texture(W, H, depth.data(), &depthSrv);
    // (0.9.570) argv[3] "sun": t42, the probe's sun mask (half size, .x visibility, .w the view depth): shadowed in its
    // left 8 texels (the screen's left 16 pixels), lit elsewhere; its bottom quarter at another depth (left out: the
    // cascades' result there)
    ID3D11ShaderResourceView* sunSrv = nullptr;
    // (0.9.571) "sun3": the same at a third of the render size (shadowed in the screen's left 16 pixels again)
    const bool sun3 = argc > 3 && std::strcmp(argv[3], "sun3") == 0;
    if (argc > 3 && (std::strcmp(argv[3], "sun") == 0 || sun3)) {
        const int k = sun3 ? 3 : 2, mw = (W + k - 1) / k, mh = (H + k - 1) / k;
        std::vector<float> sun(mw * mh * 4);
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                float* t = &sun[(y * mw + x) * 4];
                t[0] = (x + 0.5f) * W / mw < 16.0f ? 0.0f : 1.0f, t[1] = t[2] = 1.0f;
                t[3] = y < mh * 3 / 4 ? 50.0f + 5.0f * ((y + 0.5f) * H / mh - 0.5f) : 99999.0f;
            }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = mw, d.Height = mh, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_IMMUTABLE, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA s{sun.data(), UINT(mw * 16), 0};
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, &s, &t);
        dev->CreateShaderResourceView(t, nullptr, &sunSrv);
    }
    // t43: the atlas; quadrant 0 (cascade 0) at 0,0, 1 at 0.5,0, 2 at 0,0.5, 3 at 0.5,0.5; stored depths 0 / 1
    std::vector<float> atlas(A * A);
    for (int y = 0; y < A; ++y)
        for (int x = 0; x < A; ++x) {
            const int q = (x >= A / 2 ? 1 : 0) + (y >= A / 2 ? 2 : 0);
            atlas[y * A + x] = (q == 1 || q == 3) ? 1.0f : 0.0f;
        }
    ID3D11ShaderResourceView* atlasSrv = nullptr;
    texture(A, A, atlas.data(), &atlasSrv);

    // cb2 (SceneBuffer, 86 float4) and cb10 (cbPerFrame, 4 float4)
    float cb2[86][4] = {};
    auto set = [&](int i, float a, float b, float c, float d) { cb2[i][0] = a, cb2[i][1] = b, cb2[i][2] = c, cb2[i][3] = d; };
    set(20, 1, 0, 0, 0);                              // depth -> 1 / z
    set(46, float(W), float(H), 0, 0);                // (0.9.571) ScreenResolution: the sun mask's scale
    set(45, -1, 1, 2.0f / W, -2.0f / H);              // pixel -> ndc
    set(18, 1, 1, 0, 0);                              // ndc -> view x / z, y / z
    set(85, 0, 0, 0, 0);                              // (stereo)
    set(47, 1, 1, 0, 1);                              // cascade 1: Lin (and .w: cascade 0's, 1)
    set(48, 0.5f, 0, 0, 0);                           // cascade 1: Const (quadrant 1) (and .w: cascade 0's, 0)
    set(49, 1, 1, 0, 0);
    set(50, 0, 0.5f, 0, 0);                           // cascade 2: quadrant 2
    set(51, 1, 1, 0, 0);
    set(52, 0.5f, 0.5f, 0, 0);                        // cascade 3: quadrant 3
    // (Lin.z 0 and Const.z 0: the compare depth stays r1.z * 0 = 0 ... set below so that it is 0.5 everywhere)
    cb2[47][2] = cb2[49][2] = cb2[51][2] = 0.0f;
    cb2[48][2] = cb2[50][2] = cb2[52][2] = 0.5f;
    set(53, 1200, 800, 400, 200);                     // ends: the farthest first
    set(70, 1e6f, 1e6f, 1e6f, 1e6f);                  // extents
    set(71, 0, 0, 1.0f / A, 1.0f / A);                // texel size
    float cb10[4][4] = {};
    // r1 = x * cb10[0] + y * cb10[1] + z * cb10[2] + cb10[3]: cascade 0's map in quadrant 0 (u, v = 0.25 + x / 5600),
    // the compare depth 0.5, w = z
    cb10[0][0] = 1.0f / 5600.0f;
    cb10[1][1] = 1.0f / 5600.0f;
    cb10[2][3] = 1.0f;
    cb10[3][0] = 0.25f, cb10[3][1] = 0.25f, cb10[3][2] = 0.5f;
    auto buffer = [&](const void* data, UINT size) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = size, d.Usage = D3D11_USAGE_IMMUTABLE, d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA s{data, 0, 0};
        ID3D11Buffer* b = nullptr;
        dev->CreateBuffer(&d, &s, &b);
        return b;
    };
    ID3D11Buffer* b2 = buffer(cb2, sizeof(cb2));
    ID3D11Buffer* b10 = buffer(cb10, sizeof(cb10));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr;
    dev->CreateSamplerState(&sd, &smp);

    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = W, rd.Height = H, rd.MipLevels = 1, rd.ArraySize = 1, rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1, rd.Usage = D3D11_USAGE_DEFAULT, rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    dev->CreateTexture2D(&rd, nullptr, &rt);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    rd.Usage = D3D11_USAGE_STAGING, rd.BindFlags = 0, rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&rd, nullptr, &staging);

    D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(1, 1, &depthSrv);
    ctx->PSSetShaderResources(43, 1, &atlasSrv);
    if (sunSrv) ctx->PSSetShaderResources(42, 1, &sunSrv);   // (0.9.570)
    ctx->PSSetConstantBuffers(2, 1, &b2);
    ctx->PSSetConstantBuffers(10, 1, &b10);
    ctx->PSSetSamplers(14, 1, &smp);
    ctx->Draw(3, 0);
    ctx->CopyResource(staging, rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    std::FILE* out = std::fopen(argv[2], "w");
    for (int y = 0; y < H; ++y) {
        const float* row = reinterpret_cast<const float*>(static_cast<const char*>(m.pData) + y * m.RowPitch);
        std::fprintf(out, "%.0f %.4f %.4f\n", 50.0f + 5.0f * y, row[W / 2], row[2]);   // z, the centre, near the left
    }
    std::fclose(out);
    ctx->Unmap(staging, 0);
    std::printf("written\n");
    return 0;
}
