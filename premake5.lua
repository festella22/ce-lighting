-- One workspace for Spark (the Halo CE MCC mod loader, vendor/spark) and the ce-lighting mod.
-- Generate with `premake5 vs2022`, then build ce-lighting.sln.
workspace "ce-lighting"
   architecture "x64"
   configurations { "Debug", "Release" }
   platforms { "Win64" }
   startproject "ce-lighting"
   defines { "ZYDIS_STATIC_BUILD", "ASMJIT_STATIC" }

outputdir = "%{cfg.buildcfg}-%{cfg.platform}"

group "Dependencies"
   include "vendor/spark/vendor/minhook"
   include "vendor/spark/vendor/zydis"
group ""

include "vendor/spark/spark/premake5.lua"
include "vendor/spark/spark-launcher/premake5.lua"

project "ce-lighting"
    kind "SharedLib"
    language "C++"
    cppdialect "C++20"
    systemversion "latest"
    staticruntime "off" -- share the CRT heap with spark.dll

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/**.hpp",
        "src/**.cpp",
        -- Core ImGui only (for Spark's pause-menu state); spark.dll owns the DX11 backend.
        "vendor/spark/vendor/imgui/*.h",
        "vendor/spark/vendor/imgui/*.cpp",
    }

    includedirs {
        "src",
        "vendor/spark/spark/src",
        "vendor/spark/vendor/minhook/include",
        "vendor/spark/vendor/imgui",
    }

    defines { "NOMINMAX" }
    links { "spark", "d3d11", "d3dcompiler" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
