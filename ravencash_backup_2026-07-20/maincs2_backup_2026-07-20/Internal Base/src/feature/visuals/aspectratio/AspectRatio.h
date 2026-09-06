#pragma once

namespace AspectRatio {
    // Hooks ClientModeCSNormal::OverrideView and writes the current
    // build-verified CViewSetup aspect field/override flag.
    void Install();

    // Keeps the user value sane. The actual override is applied in the hook.
    void Run();
}
