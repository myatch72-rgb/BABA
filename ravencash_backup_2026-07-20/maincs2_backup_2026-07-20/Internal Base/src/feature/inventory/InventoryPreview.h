#pragma once

#include <cstdint>

struct ID3D11ShaderResourceView;

namespace InventoryPreview {

bool InstallHook();
void Shutdown();
void ResetTexture();
void ClosePanel();

void BeginFrame();
void EndFrame();
void Tick(std::uint16_t definitionIndex, int paintKit, bool autoRotate);

ID3D11ShaderResourceView *GetFrameTexture();
bool IsLive();
const char *GetStatusText();

} // namespace InventoryPreview
