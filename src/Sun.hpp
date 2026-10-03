#pragma once

// The level's sun. Halo CE keeps it in the level's sky tag: a light with a direction, color and
// power, the one the sun's lens flare is drawn for.
namespace Sun {
    struct Light {
        bool found = false;
        float dir[3]{};    // towards the sun, Halo world axes (z up)
        float color[3]{};
        float power = 0.0f;
    };

    // The current level's sun (read once per level, then cached).
    const Light& current();
}
