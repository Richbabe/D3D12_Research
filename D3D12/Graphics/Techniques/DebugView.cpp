#include "stdafx.h"
#include "DebugView.h"
#include "Core/ConsoleVariables.h"
#include "Graphics/ImGuiRenderer.h"
#include "Graphics/RHI/CommandContext.h"
#include "Graphics/RHI/Device.h"
#include "Graphics/RHI/PipelineState.h"
#include "Graphics/RHI/RootSignature.h"
#include "Graphics/RHI/Texture.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/SceneView.h"

#include <External/FontAwesome/IconsFontAwesome4.h>

namespace DebugViewRegistry
{
	// Function local so that Declare() is safe to call from file scope statics, before main runs.
	static std::vector<DebugViewEntry>& Storage()
	{
		static std::vector<DebugViewEntry> entries;
		return entries;
	}

	static DebugViewId& SelectedStorage()
	{
		static DebugViewId selected;
		return selected;
	}

	DebugViewId Declare(const char* pName, const DebugViewSettings& defaults, const char* pTooltip)
	{
		DebugViewId existing = Find(pName);
		if (existing.IsValid())
			return existing;

		std::vector<DebugViewEntry>& entries = Storage();
		check(entries.size() < DebugViewId::InvalidIndex, "Exceeded the maximum number of debug views");

		DebugViewEntry entry;
		entry.pName = pName;
		entry.pTooltip = pTooltip;
		entry.Defaults = defaults;
		entry.Settings = defaults;
		entries.push_back(entry);

		return DebugViewId{ (uint16)(entries.size() - 1) };
	}

	void Bind(DebugViewId id, RGTexture* pTexture)
	{
		DebugViewEntry* pEntry = Get(id);
		if (!pEntry || !pTexture)
			return;

		pEntry->pTexture = pTexture;
		pEntry->WasBound = true;
		pEntry->LastName = pTexture->GetName();
		pEntry->LastDesc = pTexture->GetDesc();
		pEntry->HasDesc = true;
	}

	std::vector<DebugViewEntry>& GetViews()
	{
		return Storage();
	}

	DebugViewEntry* Get(DebugViewId id)
	{
		std::vector<DebugViewEntry>& entries = Storage();
		return id.Index < entries.size() ? &entries[id.Index] : nullptr;
	}

	DebugViewId Find(const char* pName)
	{
		std::vector<DebugViewEntry>& entries = Storage();
		for (uint16 i = 0; i < (uint16)entries.size(); ++i)
		{
			if (_stricmp(entries[i].pName, pName) == 0)
				return DebugViewId{ i };
		}
		return DebugViewId();
	}

	DebugViewId GetSelected()
	{
		return SelectedStorage();
	}

	void SetSelected(DebugViewId id)
	{
		SelectedStorage() = id;
	}
}

namespace
{
	ConsoleCommand<const char*> gDebugViewCommand("debugview", [](const char* pName)
		{
			DebugViewId id = DebugViewRegistry::Find(pName);
			DebugViewRegistry::SetSelected(id);
			if (!id.IsValid())
				E_LOG(Warning, "No debug view named '%s', disabling debug view", pName);
		});

	constexpr const char* pColorMapNames[] =
	{
		"None",
		"Viridis",
		"Plasma",
		"Magma",
		"Inferno",
		"Turbo",
	};
	static_assert(ARRAYSIZE(pColorMapNames) == (uint32)DebugColorMap::MAX);
}

DebugViewSystem::DebugViewSystem(GraphicsDevice* pDevice)
{
	// Not on pCommonRS: its root constants are capped at 8 DWORDs, which these parameters don't fit in
	m_pRS = new RootSignature(pDevice);
	m_pRS->AddRootCBV(0);
	m_pRS->AddDescriptorTable(0, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
	m_pRS->Finalize("Debug View");

	m_pPSO = pDevice->CreateComputePipeline(m_pRS, "DebugView.hlsl", "DebugViewCS");
}

void DebugViewSystem::BeginFrame()
{
	for (DebugViewEntry& entry : DebugViewRegistry::GetViews())
	{
		entry.pTexture = nullptr;
		entry.WasBound = false;
	}
}

bool DebugViewSystem::IsActive() const
{
	const DebugViewEntry* pEntry = DebugViewRegistry::Get(DebugViewRegistry::GetSelected());
	return m_pPSO && pEntry && pEntry->pTexture;
}

void DebugViewSystem::Render(RGGraph& graph, RGTexture* pColorTarget)
{
	const DebugViewEntry* pEntry = DebugViewRegistry::Get(DebugViewRegistry::GetSelected());
	if (!m_pPSO || !pEntry || !pEntry->pTexture)
		return;

	RGTexture* pSource = pEntry->pTexture;
	const TextureDesc sourceDesc = pSource->GetDesc();
	const DebugViewSettings settings = pEntry->Settings;

	graph.AddPass("Debug View", RGPassFlag::Compute)
		.Read(pSource)
		.Write(pColorTarget)
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				Texture* pTarget = resources.Get(pColorTarget);

				context.SetComputeRootSignature(m_pRS);
				context.SetPipelineState(m_pPSO);

				const FormatInfo& formatInfo = RHI::GetFormatInfo(sourceDesc.Format);

				uint32 mip = (uint32)Math::Clamp(settings.MipLevel, 0, (int)sourceDesc.Mips - 1);
				Vector2u sourceSize(Math::Max(1u, sourceDesc.Width >> mip), Math::Max(1u, sourceDesc.Height >> mip));
				Vector2u outputSize(pTarget->GetWidth(), pTarget->GetHeight());

				// Letterbox the source so a target of a different resolution or aspect ratio isn't distorted
				Vector2 uvScale(1.0f, 1.0f);
				Vector2 uvBias(0.0f, 0.0f);
				if (!settings.Stretch)
				{
					float fit = Math::Min((float)outputSize.x / sourceSize.x, (float)outputSize.y / sourceSize.y);
					Vector2 displaySize(sourceSize.x * fit, sourceSize.y * fit);
					uvScale = Vector2(outputSize.x / displaySize.x, outputSize.y / displaySize.y);
					uvBias = Vector2(-(outputSize.x - displaySize.x) * 0.5f / displaySize.x, -(outputSize.y - displaySize.y) * 0.5f / displaySize.y);
				}

				uint32 channelMask = 0;
				for (uint32 i = 0; i < 4; ++i)
					channelMask |= (settings.Channels[i] ? 1u : 0u) << i;
				channelMask &= (1u << formatInfo.NumComponents) - 1u;

				struct
				{
					Vector2u	OutputDimensions;
					Vector2		UVScale;
					Vector2		UVBias;
					Vector2		ValueRange;
					uint32		TextureSource;
					uint32		TextureType;
					uint32		ChannelMask;
					uint32		MipLevel;
					uint32		Slice;
					uint32		IsIntegerFormat;
					uint32		ColorMap;
				} parameters;

				parameters.OutputDimensions	= outputSize;
				parameters.UVScale			= uvScale;
				parameters.UVBias			= uvBias;
				parameters.ValueRange		= Vector2(settings.RangeMin, settings.RangeMax);
				parameters.TextureSource	= resources.GetSRV(pSource)->GetHeapIndex();
				parameters.TextureType		= (uint32)sourceDesc.Type;
				parameters.ChannelMask		= channelMask;
				parameters.MipLevel			= mip;
				parameters.Slice			= (uint32)Math::Clamp(settings.Slice, 0.0f, (float)sourceDesc.DepthOrArraySize - 1);
				parameters.IsIntegerFormat	= formatInfo.Type == FormatType::Integer;
				parameters.ColorMap			= (uint32)settings.ColorMap;

				context.BindRootCBV(0, parameters);
				context.BindResources(1, pTarget->GetUAV());

				context.Dispatch(ComputeUtils::GetNumThreadGroups(outputSize.x, 8, outputSize.y, 8));
			});
}

void DebugViewSystem::DrawUI()
{
	if (ImGui::Begin("Parameters"))
	{
		if (ImGui::CollapsingHeader("Debug View"))
		{
			ImGui::PushID("DebugView");

			std::vector<DebugViewEntry>& views = DebugViewRegistry::GetViews();
			DebugViewId selected = DebugViewRegistry::GetSelected();

			// Index 0 is "Off", so view i sits at combo index i + 1
			std::vector<const char*> names;
			names.reserve(views.size() + 1);
			names.push_back("Off");
			for (const DebugViewEntry& entry : views)
				names.push_back(entry.pName);

			int current = selected.IsValid() ? selected.Index + 1 : 0;
			if (ImGui::Combo("View", &current, names.data(), (int)names.size()))
				DebugViewRegistry::SetSelected(current == 0 ? DebugViewId() : DebugViewId{ (uint16)(current - 1) });

			DebugViewEntry* pEntry = DebugViewRegistry::Get(DebugViewRegistry::GetSelected());
			if (pEntry)
			{
				if (pEntry->pTooltip)
					ImGui::TextDisabled("%s", pEntry->pTooltip);

				DebugViewSettings& settings = pEntry->Settings;
				const TextureDesc& desc = pEntry->LastDesc;
				const FormatInfo& formatInfo = RHI::GetFormatInfo(desc.Format);
				const uint32 numComponents = pEntry->HasDesc ? formatInfo.NumComponents : 4;

				// Channel visibility switches
				{
					ImVec2 buttonSize = ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing());
					const char* pChannelNames[] = { "R", "G", "B", "A" };
					ImGui::AlignTextToFramePadding();
					ImGui::Text("Channels");
					for (uint32 i = 0; i < 4; ++i)
					{
						ImGui::SameLine();
						ImGui::BeginDisabled(numComponents <= i);
						ImGui::ToggleButton(pChannelNames[i], &settings.Channels[i], buttonSize);
						ImGui::EndDisabled();
					}
				}

				ImGui::RangeSlider("Range", &settings.RangeMin, &settings.RangeMax);

				ImGui::BeginDisabled(desc.Mips <= 1);
				ImGui::SliderInt("Mip", &settings.MipLevel, 0, Math::Max(0, (int)desc.Mips - 1));
				ImGui::EndDisabled();

				ImGui::BeginDisabled(desc.Type != TextureType::Texture3D);
				ImGui::SliderFloat("Slice", &settings.Slice, 0.0f, Math::Max(0.0f, (float)desc.DepthOrArraySize - 1), "%.2f");
				ImGui::EndDisabled();

				int colorMap = (int)settings.ColorMap;
				if (ImGui::Combo("Color Map", &colorMap, pColorMapNames, ARRAYSIZE(pColorMapNames)))
					settings.ColorMap = (DebugColorMap)colorMap;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Only applies when a single channel is visible");

				ImGui::Checkbox("Stretch to viewport", &settings.Stretch);

				if (ImGui::Button(ICON_FA_RECYCLE " Reset"))
					settings = pEntry->Defaults;

				if (!pEntry->WasBound)
					ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Not rendered - enable the technique that produces it");
				else
					ImGui::TextDisabled("%s - %dx%d - %d mips - %s", pEntry->LastName.c_str(), desc.Width, desc.Height, desc.Mips, formatInfo.pName);
			}

			ImGui::PopID();
		}
	}
	ImGui::End();
}
