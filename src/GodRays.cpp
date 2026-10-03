#include "GodRays.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include "D3DState.hpp"
#include "Log.hpp"
#include "Sun.hpp"

namespace GodRays {
    namespace {
        // Halo's level shader keeps its view-projection in the first 4 rows of vertex constant
        // buffer 0 (row-major, Halo world space, reversed infinite depth: the sky is depth 0).
        constexpr UINT kViewProjBytes = 64;

        // Tuning: how bright the sky must be to shine, how far the shafts reach, how fast they fade,
        // how strong each sample is, how strong the shafts are over the image.
        constexpr float kThreshold = 0.35f, kDensity = 0.92f, kDecay = 0.965f, kWeight = 0.045f, kIntensity = 1.0f;

        struct alignas(16) LightCB {
            float sunDir[4];    // towards the sun, Halo world axes
            float sunColor[4];  // rgb, a = intensity
            float params[4];    // threshold, density, decay, weight
            float screen[4];    // width, height, debug (1/0), width / height
        };

        constexpr char kShader[] = R"(
cbuffer Light : register(b0) { float4 sunDir; float4 sunColor; float4 params; float4 screen; };
cbuffer Halo : register(b1) { float4 vp0; float4 vp1; float4 vp2; float4 vp3; };
Texture2D scene : register(t0);
Texture2D<float> depth : register(t1);
Texture2D src : register(t2);
SamplerState lin : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
// The sun's place on screen (a direction projects like a point at infinity), and how much the
// camera faces it: 1 looking straight at it, 0 side-on or away.
float2 SunUV(out float facing) {
	float w = dot(vp3.xyz, sunDir.xyz);
	facing = saturate(w / length(vp3.xyz));
	float2 ndc = float2(dot(vp0.xyz, sunDir.xyz), dot(vp1.xyz, sunDir.xyz)) / max(w, 1e-4);
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
// What shines: open sky (nothing drawn in front of it), brightest around the sun.
float4 MaskPS(VSOut i) : SV_Target {
	if (depth.Load(int3(i.uv * screen.xy, 0)) > 0.0) return 0;
	float facing;
	float2 off = (i.uv - SunUV(facing)) * float2(screen.w, 1.0);
	float glow = exp(-dot(off, off) * 5.0);
	float3 c = scene.SampleLevel(lin, i.uv, 0).rgb;
	float bright = max(dot(c, float3(0.2126, 0.7152, 0.0722)) - params.x, 0.0);
	return float4((c * bright + 0.12) * glow, 1.0);
}
// Each pixel gathers what shines between it and the sun, fading with distance.
float4 RaysPS(VSOut i) : SV_Target {
	float facing;
	float2 sun = SunUV(facing);
	const int N = 64;
	float2 stepUV = (i.uv - sun) * params.y / N;
	float2 uv = i.uv;
	float decay = 1.0;
	float3 sum = 0;
	[loop] for (int s = 0; s < N; ++s) {
		uv -= stepUV;
		sum += src.SampleLevel(lin, uv, 0).rgb * decay;
		decay *= params.z;
	}
	return float4(sum * params.w * facing, 1.0);
}
float4 CompositePS(VSOut i) : SV_Target {
	float3 r = src.SampleLevel(lin, i.uv, 0).rgb;
	return float4(screen.z > 0.5 ? r : r * sunColor.rgb * sunColor.a, 1.0);
}
)";

        template <class T> void rel(T*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader *psMask = nullptr, *psRays = nullptr, *psComposite = nullptr;
        ID3D11Buffer *lightCb = nullptr, *viewProjCb = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11BlendState *opaque = nullptr, *additive = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* noDepth = nullptr;
        bool initFailed = false;

        // Halo's scene depth, captured when it draws its level, and copies we can read.
        ID3D11Texture2D* haloDepth = nullptr;
        bool sceneSeen = false;

        struct Target {
            ID3D11Texture2D* tex = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            UINT w = 0, h = 0;
            void drop() { rel(rtv), rel(srv), rel(tex), w = h = 0; }
        };
        Target sceneCopy, depthCopy, mask, rays;

        // A texture we can read (and, with renderTarget, draw to).
        bool ensure(Target& t, UINT w, UINT h, DXGI_FORMAT format, DXGI_FORMAT srvFormat, bool renderTarget) {
            if (t.tex && t.w == w && t.h == h)
                return true;
            t.drop();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = srvFormat;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex)) || FAILED(device->CreateShaderResourceView(t.tex, &sv, &t.srv)) ||
                (renderTarget && FAILED(device->CreateRenderTargetView(t.tex, nullptr, &t.rtv)))) {
                t.drop();
                return false;
            }
            t.w = w;
            t.h = h;
            return true;
        }

        bool init(IDXGISwapChain* swapChain) {
            if (vs)
                return true;
            if (initFailed || FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
                return false;
            device->GetImmediateContext(&context);
            auto compile = [&](const char* entry, const char* target) -> ID3DBlob* {
                ID3DBlob *blob = nullptr, *errors = nullptr;
                if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "ce_lighting_godrays", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
                    log(std::string("shader ") + entry + " failed: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
                rel(errors);
                return blob;
            };
            auto pixel = [&](const char* entry, ID3D11PixelShader** out) {
                if (ID3DBlob* blob = compile(entry, "ps_5_0")) {
                    device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
                    blob->Release();
                }
            };
            if (ID3DBlob* blob = compile("VSMain", "vs_5_0")) {
                device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
                blob->Release();
            }
            pixel("MaskPS", &psMask);
            pixel("RaysPS", &psRays);
            pixel("CompositePS", &psComposite);

            D3D11_BUFFER_DESC cbd{};
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            cbd.ByteWidth = sizeof(LightCB);
            device->CreateBuffer(&cbd, nullptr, &lightCb);
            cbd.Usage = D3D11_USAGE_DEFAULT;  // a copy destination, never touched by the CPU
            cbd.CPUAccessFlags = 0;
            cbd.ByteWidth = kViewProjBytes;
            device->CreateBuffer(&cbd, nullptr, &viewProjCb);

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &sampler);

            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &opaque);
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;  // light adds up
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
            device->CreateBlendState(&bd, &additive);

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            device->CreateRasterizerState(&rd, &raster);
            D3D11_DEPTH_STENCIL_DESC dd{};  // no depth test or write
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            device->CreateDepthStencilState(&dd, &noDepth);

            const bool ok = vs && psMask && psRays && psComposite && lightCb && viewProjCb && sampler && opaque && additive && raster && noDepth;
            log(ok ? "god rays ready" : "god rays failed to initialize");
            initFailed = !ok;
            return ok;
        }

        void pass(ID3D11RenderTargetView* target, UINT w, UINT h, ID3D11PixelShader* ps, ID3D11BlendState* blend) {
            const float factor[4]{};
            const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
            context->OMSetRenderTargets(1, &target, nullptr);
            context->RSSetViewports(1, &vp);
            context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
            context->PSSetShader(ps, nullptr, 0);
            context->Draw(3, 0);
        }
    }

    void onSceneRendered() {
        if (!vs)
            return;
        // Halo's view-projection, exactly as it drew this frame's level: GPU-to-GPU, no stall.
        ID3D11Buffer* haloCb = nullptr;
        context->VSGetConstantBuffers(0, 1, &haloCb);
        if (!haloCb)
            return;
        const D3D11_BOX box{ 0, 0, 0, kViewProjBytes, 1, 1 };
        context->CopySubresourceRegion(viewProjCb, 0, 0, 0, 0, haloCb, 0, &box);
        rel(haloCb);

        // And its depth buffer (kept until Halo swaps it for another).
        ID3D11DepthStencilView* dsv = nullptr;
        context->OMGetRenderTargets(0, nullptr, &dsv);
        if (dsv) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(&tex)) && tex != haloDepth) {
                rel(haloDepth);
                haloDepth = tex;
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                log(std::format("Halo depth {}x{} format {} samples {}", d.Width, d.Height, int(d.Format), d.SampleDesc.Count));
            } else {
                rel(tex);
            }
            rel(res);
            rel(dsv);
        }
        sceneSeen = haloDepth != nullptr;
    }

    void draw(IDXGISwapChain* swapChain) {
        if (!init(swapChain))
            return;
        const bool drawn = sceneSeen;
        sceneSeen = false;
        const Sun::Light& sun = Sun::current();
        if (!drawn || !sun.found || mode == Mode::Off)
            return;

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
            return;
        D3D11_TEXTURE2D_DESC bb{}, dd{};
        backBuffer->GetDesc(&bb);
        haloDepth->GetDesc(&dd);
        ID3D11RenderTargetView* backRtv = nullptr;
        // ponytail: no effect when Halo renders at another resolution than the window, or with MSAA
        const bool ok = bb.Width == dd.Width && bb.Height == dd.Height && dd.SampleDesc.Count == 1 && bb.SampleDesc.Count == 1 &&
                        SUCCEEDED(device->CreateRenderTargetView(backBuffer, nullptr, &backRtv)) &&
                        ensure(sceneCopy, bb.Width, bb.Height, bb.Format, bb.Format, false) &&
                        ensure(depthCopy, dd.Width, dd.Height, dd.Format, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, false) &&
                        ensure(mask, bb.Width / 2, bb.Height / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
                        ensure(rays, bb.Width / 2, bb.Height / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        if (!ok) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                log(std::format("god rays off: back buffer {}x{} format {}, depth {}x{} format {}", bb.Width, bb.Height, int(bb.Format), dd.Width, dd.Height, int(dd.Format)));
            }
            rel(backRtv);
            rel(backBuffer);
            return;
        }

        D3DState saved;
        saved.save(context);
        context->OMSetRenderTargets(0, nullptr, nullptr);  // Halo's depth may still be bound
        context->CopyResource(sceneCopy.tex, backBuffer);
        context->CopyResource(depthCopy.tex, haloDepth);
        rel(backBuffer);

        LightCB cb{};
        std::memcpy(cb.sunDir, sun.dir, sizeof(sun.dir));
        std::memcpy(cb.sunColor, sun.color, sizeof(sun.color));
        cb.sunColor[3] = kIntensity;
        cb.params[0] = kThreshold, cb.params[1] = kDensity, cb.params[2] = kDecay, cb.params[3] = kWeight;
        cb.screen[0] = float(bb.Width), cb.screen[1] = float(bb.Height);
        cb.screen[2] = mode == Mode::Debug ? 1.0f : 0.0f;
        cb.screen[3] = float(bb.Width) / float(bb.Height);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof(cb));
            context->Unmap(lightCb, 0);
        }

        ID3D11Buffer* cbs[2] = { lightCb, viewProjCb };
        context->PSSetConstantBuffers(0, 2, cbs);
        context->VSSetShader(vs, nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(raster);
        context->OMSetDepthStencilState(noDepth, 0);
        context->PSSetSamplers(0, 1, &sampler);
        ID3D11ShaderResourceView* none = nullptr;

        ID3D11ShaderResourceView* inputs[2] = { sceneCopy.srv, depthCopy.srv };
        context->PSSetShaderResources(0, 2, inputs);
        pass(mask.rtv, mask.w, mask.h, psMask, opaque);
        context->OMSetRenderTargets(0, nullptr, nullptr);  // a texture can't be read while it's the target
        context->PSSetShaderResources(2, 1, &mask.srv);
        pass(rays.rtv, rays.w, rays.h, psRays, opaque);
        context->PSSetShaderResources(2, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(2, 1, &rays.srv);
        pass(backRtv, bb.Width, bb.Height, psComposite, mode == Mode::Debug ? opaque : additive);
        context->PSSetShaderResources(2, 1, &none);

        saved.restore(context);
        rel(backRtv);
    }

    void release() {
        for (auto* t : { &sceneCopy, &depthCopy, &mask, &rays })
            t->drop();
        rel(haloDepth), rel(vs), rel(psMask), rel(psRays), rel(psComposite), rel(lightCb), rel(viewProjCb);
        rel(sampler), rel(opaque), rel(additive), rel(raster), rel(noDepth), rel(context), rel(device);
        initFailed = false;
    }
}
