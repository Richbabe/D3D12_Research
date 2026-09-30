#pragma once

#include "RHI/RHI.h"
#include "RenderGraph/RenderGraphDefinitions.h"

using WindowHandle = HWND;

namespace ImGui
{
	ImVec2 GetAutoSize(const ImVec2& dimensions);
	bool ToggleButton(const char* pText, bool* pValue, const ImVec2& size = ImVec2(0, 0));
	void AddText(ImDrawList* pDrawList, const char* pText, ImVec2 pos, ImU32 inColor, float angleRadians);

	// Dual handle slider defining a [min, max] sub-range of [limitMin, limitMax], with a black-to-white preview ramp.
	bool RangeSlider(const char* pLabel, float* pMin, float* pMax, float limitMin = 0.0f, float limitMax = 1.0f);
}

namespace ImGuiRenderer
{
	void Initialize(GraphicsDevice* pDevice, WindowHandle window);
	void Shutdown();

	void NewFrame();
	void Render(CommandContext& context, Texture* pRenderTarget);
	void PresentViewports();
};

