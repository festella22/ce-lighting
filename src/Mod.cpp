// ce-lighting: better lighting for Halo CE on MCC, as a Spark mod. Spark LoadLibrary-s every DLL
// in MCC\Binaries\Win64\mods\ and calls spark_modLoad once a Halo CE level is running.
#include <Windows.h>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "GodRays.hpp"
#include "Log.hpp"
#include "imgui.h"
#include "spark/SparkAPI.h"
#include "spark/hook/Hooks.hpp"
#include "spark/mod/IMod.hpp"
#include "spark/mod/ImGuiBridge.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

void log(const std::string& msg) {
    static std::ofstream file = [] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
        return std::ofstream(std::filesystem::path(path).replace_extension(".log"));
    }();
    std::cout << "[ce-lighting] " << msg << std::endl;
    file << msg << std::endl;
}

namespace {
    // Present, through the vtable every DXGI swap chain shares (a throwaway swap chain finds it).
    // Patching the vtable, not the code, chains with Spark's own hook and other mods that do the
    // same (HaloCraft); Spark frees mods in reverse load order, so the chain unwinds cleanly.
    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, const void*);
    constexpr int kPresent = 8, kPresent1 = 22;  // IDXGISwapChain / IDXGISwapChain1 vtable slots
    void** vtable = nullptr;
    PresentFn originalPresent = nullptr;
    Present1Fn originalPresent1 = nullptr;

    // F7: god rays on/off. Ctrl+F7: the shafts alone (to see what they come from).
    void keys() {
        static bool wasDown = false;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (down && !wasDown) {
            using GodRays::Mode;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                GodRays::mode = GodRays::mode == Mode::Debug ? Mode::On : Mode::Debug;
            else
                GodRays::mode = GodRays::mode == Mode::Off ? Mode::On : Mode::Off;
            log(GodRays::mode == Mode::Off ? "god rays off" : GodRays::mode == Mode::On ? "god rays on" : "god rays: debug view");
        }
        wasDown = down;
    }

    void frame(IDXGISwapChain* swapChain) {
        keys();
        // Spark sets ImGui's WantCaptureMouse exactly while Halo's pause menu is open: leave it be.
        Spark::Mod::syncImGuiContext();
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
            return;
        GodRays::draw(swapChain);
    }

    HRESULT WINAPI presentHook(IDXGISwapChain* swapChain, UINT sync, UINT flags) {
        frame(swapChain);
        return originalPresent(swapChain, sync, flags);
    }

    HRESULT WINAPI present1Hook(IDXGISwapChain* swapChain, UINT sync, UINT flags, const void* params) {
        frame(swapChain);
        return originalPresent1(swapChain, sync, flags, params);
    }

    void patch(int slot, void* fn) {
        DWORD old = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vtable[slot] = fn;
        VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    }

    void hookPresent() {
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"ce-lighting", 0, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        IDXGISwapChain* swapChain = nullptr;
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, nullptr))) {
            vtable = *reinterpret_cast<void***>(swapChain);
            originalPresent = reinterpret_cast<PresentFn>(vtable[kPresent]);
            originalPresent1 = reinterpret_cast<Present1Fn>(vtable[kPresent1]);
            patch(kPresent, reinterpret_cast<void*>(&presentHook));
            patch(kPresent1, reinterpret_cast<void*>(&present1Hook));
            swapChain->Release();
            device->Release();
        }
        DestroyWindow(hwnd);
        log(vtable ? "Present hooked" : "Present hook failed");
    }

    class LightingMod : public Spark::IMod {
    public:
        void init() override {
            log("ce-lighting loaded (F7: god rays on/off)");
            hookPresent();
            Spark::RenderBSPAlbedo::addHandler(modId_, +[](void*, Spark::RenderBSPAlbedo::Cursor next) {
                next();
                GodRays::onSceneRendered();
            }, nullptr);
        }

        void free() override {
            if (vtable) {
                patch(kPresent, reinterpret_cast<void*>(originalPresent));
                patch(kPresent1, reinterpret_cast<void*>(originalPresent1));
                vtable = nullptr;
            }
            GodRays::release();
        }
    };
}

extern "C" __declspec(dllexport) void spark_modLoad() {
    spark_registerMod(new LightingMod());
}
