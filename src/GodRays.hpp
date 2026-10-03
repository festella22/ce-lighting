#pragma once
#include <d3d11.h>

// Light shafts from the level's sun: the open sky around the sun (Halo's depth buffer says what's
// sky) streaked out from the sun's place on screen, so cliffs, arches and the ring's edge cut
// beams out of it. The screen-space method from GPU Gems 3, ch. 13.
namespace GodRays {
    enum class Mode { Off, On, Debug };  // Debug: only the shafts, on black
    inline Mode mode = Mode::On;

    void onSceneRendered();                       // after Halo draws its level (render thread)
    void draw(IDXGISwapChain* swapChain);         // at Present, onto the finished frame
    void release();
}
