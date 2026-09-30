#pragma once

#include "RHI/RHI.h"
#include "RHI/Texture.h"
#include "RenderGraph/RenderGraphDefinitions.h"

// Must match the ColorMap_* constants in Shaders/DebugView.hlsl
enum class DebugColorMap : uint8
{
	None,
	Viridis,
	Plasma,
	Magma,
	Inferno,
	Turbo,
	MAX
};

struct DebugViewSettings
{
	bool			Channels[4]	= { true, true, true, true };
	float			RangeMin	= 0.0f;
	float			RangeMax	= 1.0f;
	int				MipLevel	= 0;
	float			Slice		= 0.0f;
	DebugColorMap	ColorMap	= DebugColorMap::None;
	bool			Stretch		= false;	// false keeps the source aspect ratio and letterboxes
};

struct DebugViewId
{
	static constexpr uint16 InvalidIndex = 0xFFFF;

	uint16 Index = InvalidIndex;
	bool IsValid() const { return Index != InvalidIndex; }
};

struct DebugViewEntry
{
	const char*			pName		= nullptr;
	const char*			pTooltip	= nullptr;
	DebugViewSettings	Defaults;
	DebugViewSettings	Settings;

	// Rebound every frame by whichever technique produced the texture. Only valid during graph recording:
	// the graph is a per-frame stack object, so the UI must never dereference this.
	RGTexture*			pTexture	= nullptr;

	// Mirrors of the bound texture that outlive the graph, for the UI to read.
	bool				WasBound	= false;
	String				LastName;
	TextureDesc			LastDesc;
	bool				HasDesc		= false;
};

namespace DebugViewRegistry
{
	// Declares a view up front, typically from a file scope static in the technique that produces it.
	// Declaring an existing name returns the existing ID, so interchangeable techniques (SSAO and RTAO,
	// for instance) share one view and whichever ran that frame binds it.
	DebugViewId Declare(const char* pName, const DebugViewSettings& defaults, const char* pTooltip = nullptr);

	// Publishes the texture backing a view. Call during render graph recording, every frame.
	void Bind(DebugViewId id, RGTexture* pTexture);

	Array<DebugViewEntry>& GetViews();
	DebugViewEntry* Get(DebugViewId id);
	DebugViewId Find(const char* pName);

	DebugViewId GetSelected();
	void SetSelected(DebugViewId id);
}

class DebugViewSystem
{
public:
	explicit DebugViewSystem(GraphicsDevice* pDevice);

	// Drops last frame's bindings. Call right after the render graph is created.
	void BeginFrame();

	bool IsActive() const;

	// Overwrites pColorTarget with the selected view. Expects a post-tonemap, display-referred target.
	void Render(RGGraph& graph, RGTexture* pColorTarget);

	// Appends a section to the "Settings" window. Call during graph recording, after the techniques have
	// added their own sections, so this ends up last in the panel and sees this frame's bindings.
	void DrawUI();

private:
	Ref<PipelineState> m_pPSO;
};
