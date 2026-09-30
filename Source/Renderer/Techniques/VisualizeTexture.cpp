#include "stdafx.h"
#include "VisualizeTexture.h"
#include "Core/Image.h"
#include "RHI/Device.h"
#include "RHI/RHI.h"
#include "RHI/PipelineState.h"
#include "RHI/RootSignature.h"
#include "Renderer/Techniques/ImGuiRenderer.h"
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderGraph.h"

#include <imgui_internal.h>
#include <IconsFontAwesome4.h>

struct PickingData
{
	Vector4 DataFloat;
	Vector4u DataUInt;
};

CaptureTextureSystem::CaptureTextureSystem(GraphicsDevice* pDevice)
{
	m_pVisualizePSO = pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, "ImageVisualize.hlsl", "CSMain");
}


void CaptureTextureSystem::Capture(RGGraph& graph, CaptureTextureContext& captureContext, RGTexture* pSource)
{
	if (!pSource)
		return;

	captureContext.SourceName = pSource->GetName();
	captureContext.SourceDesc = pSource->GetDesc();

	RGBuffer* pReadbackTarget = RGUtils::CreatePersistent(graph, "TextureCapture.ReadbackTarget", BufferDesc::CreateReadback(sizeof(Vector4u) * 2), &captureContext.pReadbackBuffer);

	const TextureDesc& desc = pSource->GetDesc();
	Vector2u mipSize(desc.Width >> captureContext.MipLevel, desc.Height >> captureContext.MipLevel);
	RGTexture* pTarget = RGUtils::CreatePersistent(graph, "TextureCapture.Target", TextureDesc::Create2D(mipSize.x, mipSize.y, ResourceFormat::RGBA8_UNORM, 1, TextureFlag::ShaderResource), &captureContext.pTextureTarget);
	RGBuffer* pPickingBuffer = graph.Create("TextureCapture.Picking", BufferDesc::CreateStructured(1, sizeof(Vector4u)));

	graph.AddPass("CaptureTexture.Process", RGPassFlag::Compute)
		.Read({ pSource })
		.Write({ pTarget, pPickingBuffer })
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
				context.SetPipelineState(m_pVisualizePSO);

				struct ConstantsData
				{
					Vector2u HoveredPixel;
					Vector2u Dimensions;
					Vector2 ValueRange;
					TextureView TextureSource;
					RWTextureView TextureTarget;
					uint32 TextureType;
					uint32 ChannelMask;
					uint32 MipLevel;
					uint32 Slice;
					uint32 IsIntegerFormat;
					uint32 IntAsID;

					RWBufferView PickingBuffer;
				} constants;

				const FormatInfo& formatInfo = RHI::GetFormatInfo(desc.Format);

				constants.HoveredPixel		= captureContext.HoveredPixel;
				constants.TextureSource		= resources.GetSRV(pSource);
				constants.TextureTarget		= resources.GetUAV(pTarget);
				constants.Dimensions		= mipSize;
				constants.TextureType		= (uint32)pSource->GetDesc().Type;
				constants.ValueRange		= Vector2(captureContext.RangeMin, captureContext.RangeMax);
				constants.ChannelMask		=	(captureContext.VisibleChannels[0] ? 1 : 0) << 0 |
												(captureContext.VisibleChannels[1] ? 1 : 0) << 1 |
												(captureContext.VisibleChannels[2] ? 1 : 0) << 2 |
												(captureContext.VisibleChannels[3] ? 1 : 0) << 3;
				constants.ChannelMask		&= ((1u << formatInfo.NumComponents) - 1u);
				constants.MipLevel			= (uint32)captureContext.MipLevel;
				constants.Slice				= desc.Type == TextureType::TextureCube ? captureContext.CubeFaceIndex : (uint32)captureContext.Slice;
				constants.IsIntegerFormat	= formatInfo.Type == FormatType::Integer;
				constants.IntAsID			= captureContext.IntAsID;
				constants.PickingBuffer		= resources.GetUAV(pPickingBuffer);

				context.BindRootSRV(BindingSlot::PerInstance, constants);

				context.Dispatch(ComputeUtils::GetNumThreadGroups(desc.Width, 8, desc.Height, 8));
			});

	graph.AddPass("CaptureTexture.CopyPickData", RGPassFlag::Copy)
		.Read(pPickingBuffer)
		.Write(pReadbackTarget)
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				context.CopyBuffer(resources.Get(pPickingBuffer), resources.Get(pReadbackTarget), sizeof(Vector4u), 0, sizeof(Vector4u) * captureContext.ReadbackIndex);
			});

	if(captureContext.pReadbackBuffer)
		captureContext.PickingData = static_cast<Vector4u*>(captureContext.pReadbackBuffer->GetMappedData())[captureContext.ReadbackIndex];
	captureContext.ReadbackIndex = captureContext.ReadbackIndex & 1;
}

void CaptureTextureSystem::RenderUI(CaptureTextureContext& captureContext, const ImVec2& viewportOrigin, const ImVec2& viewportSize)
{
	if (captureContext.pTextureTarget)
	{
		struct Group
		{
			Group()
			{
				ImGui::BeginGroup();
				ImGui::Dummy(ImVec2(1, 3));
				ImGui::Dummy(ImVec2(0, 2));
				ImGui::SameLine();
			}

			~Group()
			{
				ImGui::SameLine();
				ImGui::Dummy(ImVec2(0, 0));
				ImGui::Dummy(ImVec2(1, 3));
				ImGui::EndGroup();
				ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImColor(0.3f, 0.3f, 0.3f, 1.0f), 2.5f);
			}
		};

		if (ImGui::Begin("Visualize Texture", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::PushID("VisualizeTexture");
			TextureDesc& desc = captureContext.SourceDesc;
			const FormatInfo& formatInfo = RHI::GetFormatInfo(desc.Format);
			Vector2u mipSize(desc.Width >> captureContext.MipLevel, desc.Height >> captureContext.MipLevel);

			ImGui::Text("%s - %dx%d %d mips - %s", captureContext.SourceName.c_str(), desc.Width, desc.Height, desc.Mips, formatInfo.pName);

			{
				Group group;

				// Channel visibility switches
				{
					auto ChannelButton = [](const char* pName, bool* pValue, bool enabled, const ImVec2& size, const ImU32& activeColor, const ImU32& inActiveColor)
					{
						ImGui::BeginDisabled(!enabled);

						ImGui::PushStyleColor(ImGuiCol_Button,			*pValue ? activeColor : inActiveColor);
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered,	*pValue ? activeColor : inActiveColor);
						ImGui::PushStyleColor(ImGuiCol_ButtonActive,	*pValue ? activeColor : inActiveColor);
						bool clicked = false;
						if (ImGui::Button(pName, size))
						{
							*pValue = !*pValue;
							clicked = true;
						}
						ImGui::PopStyleColor(3);

						ImGui::EndDisabled();
					};

					ImVec2 buttonSize = ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing());

					ChannelButton("R", &captureContext.VisibleChannels[0], formatInfo.NumComponents >= 1, buttonSize, ImColor(0.7f, 0.1f, 0.1f, 1.0f), ImColor(0.1f, 0.1f, 0.1f, 1.0f));
					ImGui::SameLine();
					ChannelButton("G", &captureContext.VisibleChannels[1], formatInfo.NumComponents >= 2, buttonSize, ImColor(0.1f, 0.7f, 0.1f, 1.0f), ImColor(0.1f, 0.1f, 0.1f, 1.0f));
					ImGui::SameLine();
					ChannelButton("B", &captureContext.VisibleChannels[2], formatInfo.NumComponents >= 3, buttonSize, ImColor(0.1f, 0.1f, 0.7f, 1.0f), ImColor(0.1f, 0.1f, 0.1f, 1.0f));
					ImGui::SameLine();
					ChannelButton("A", &captureContext.VisibleChannels[3], formatInfo.NumComponents >= 4, buttonSize, ImColor(0.8f, 0.8f, 0.8f, 1.0f), ImColor(0.1f, 0.1f, 0.1f, 1.0f));
				}

				ImGui::SameLine();

				// X-Ray mode
				{
					ImVec2 buttonSize = ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing());
					ImGui::ToggleButton(ICON_FA_SEARCH_PLUS, &captureContext.XRay, buttonSize);
				}

				ImGui::SameLine();

				// Int as ID
				{
					ImGui::BeginDisabled(formatInfo.Type != FormatType::Integer);
					ImVec2 buttonSize = ImVec2(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing());
					ImGui::ToggleButton("ID", &captureContext.IntAsID, buttonSize);
					ImGui::EndDisabled();
				}
			}

			ImGui::SameLine();

			// Mip Selection
			{
				Group group;

				ImGui::BeginDisabled(desc.Mips <= 1);
				Array<String> mipTexts(desc.Mips);
				for (uint32 i = 0; i < desc.Mips; ++i)
				{
					mipTexts[i] = Sprintf("%d - %dx%d", i, Math::Max(1u, desc.Width >> i), Math::Max(1u, desc.Height >> i));
				}
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Mip");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(170);

				ImGui::Combo("##Mip", &captureContext.MipLevel, [](void* pData, int idx)
					{
						String* pStrings = (String*)pData;
						return pStrings[idx].c_str();
					}, mipTexts.data(), (int)mipTexts.size());
				ImGui::EndDisabled();
			}

			ImGui::SameLine();

			// Slice Selection
			{
				Group group;

				ImGui::BeginDisabled(desc.Type != TextureType::Texture3D);
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Slice");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100);
				ImGui::SliderFloat("##SliceNr", &captureContext.Slice, 0, (float)desc.Depth - 1, "%.2f");
				ImGui::EndDisabled();
			}

			// Face Selection
			ImGui::SameLine();
			{
				Group group;

				ImGui::BeginDisabled(desc.Type != TextureType::TextureCube);
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Face");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100);

				constexpr const char* pFaces[] = {
					"Right",
					"Left",
					"Top",
					"Bottom",
					"Front",
					"Back",
				};
				ImGui::Combo("##Face", &captureContext.CubeFaceIndex, pFaces, ARRAYSIZE(pFaces));
				ImGui::EndDisabled();
			}

			// Zoom/Scale
			{
				Group group;

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Zoom");
				ImGui::SameLine();
				if (ImGui::Button("1:1"))
				{
					captureContext.Scale = 1.0f;
				}
				ImGui::SameLine();
				if (ImGui::Button(ICON_FA_ARROWS_ALT " Fit"))
				{
					ImVec2 ratio = ImGui::GetWindowSize() / ImVec2((float)mipSize.x, (float)mipSize.y);
					captureContext.Scale = ImMin(ratio.x, ratio.y);
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(60);
				float scalePercent = captureContext.Scale * 100.0f;
				if (ImGui::DragFloat("##Scale", &scalePercent, 4.0f, 1.0f, 50000.0f, "%.0f%%", ImGuiSliderFlags_Logarithmic))
				{
					captureContext.Scale = scalePercent / 100.0f;
				}
			}
			

			ImGui::SameLine();

			// Value Range Slider
			{
				Group group;

				ImGui::RangeSlider("Range", &captureContext.RangeMin, &captureContext.RangeMax);
			}


			// Image
			{
				ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollWithMouse;
				if (captureContext.XRay)
				{
					windowFlags |= ImGuiWindowFlags_NoScrollbar;
				}
				else
				{
					windowFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
					windowFlags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
				}

				ImVec2 avail = ImGui::GetContentRegionAvail();
				ImGui::BeginChild("##ImageView", ImVec2(avail.x, avail.y), ImGuiChildFlags_None, windowFlags);

				ImVec2 UV;
				ImVec2 imageSize = ImVec2((float)mipSize.x, (float)mipSize.y) * captureContext.Scale;

				// Add checkerboard background
				ImVec2 checkersSize = ImMax(ImGui::GetContentRegionAvail(), imageSize);
				ImVec2 c = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddImage((ImTextureID)GraphicsCommon::GetDefaultTexture(DefaultTexture::CheckerPattern), c, c + ImGui::GetContentRegionAvail(), ImVec2(0.0f, 0.0f), checkersSize / 50.0f, ImColor(0.1f, 0.1f, 0.1f, 1.0f));

				bool imageHovered = false;
				if (captureContext.XRay)
				{
					// Match image with viewport
					const ImRect bb(c, c + ImGui::GetContentRegionAvail());
					ImGui::ItemSize(bb);
					if (ImGui::ItemAdd(bb, ImGui::GetID("##Image")))
					{
						ImGui::GetWindowDrawList()->AddImage((ImTextureID)captureContext.pTextureTarget.Get(), viewportOrigin, viewportOrigin + viewportSize);
					}

					imageHovered = ImGui::IsItemHovered();
					UV = (ImGui::GetMousePos() - viewportOrigin) / viewportSize;
				}
				else
				{
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::ImageButton("##Image", (ImTextureID)captureContext.pTextureTarget.Get(), imageSize);
					ImGui::PopStyleVar();

					imageHovered = ImGui::IsItemHovered();
					UV = (ImGui::GetMousePos() - ImGui::GetItemRectMin()) / ImGui::GetItemRectSize();

					if (imageHovered)
					{
						// Panning
						if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
						{
							ImGuiContext& g = *ImGui::GetCurrentContext();
							ImGuiWindow* window = g.CurrentWindow;
							ImGui::SetScrollX(window, window->Scroll.x - ImGui::GetIO().MouseDelta.x);
							ImGui::SetScrollY(window, window->Scroll.y - ImGui::GetIO().MouseDelta.y);
						}

						// Zooming
						float wheel = ImGui::GetIO().MouseWheel;
						if (wheel != 0)
						{
							float logScale = logf(captureContext.Scale);
							logScale += wheel / 5.0f;
							captureContext.Scale = Math::Clamp(expf(logScale), 0.0f, 1000.0f);
						}
					}
				}

				ImGui::EndChild();

				captureContext.HoveredPixel = Vector2u((uint32)Math::Floor(UV.x * mipSize.x), (uint32)Math::Floor(UV.y * mipSize.y));

				// Pixel Data
				{
					if (imageHovered)
					{
						UV = ImClamp(UV, ImVec2(0, 0), ImVec2(1, 1));
						Vector2u texel((uint32)Math::Floor(UV.x * mipSize.x), (uint32)Math::Floor(UV.y * mipSize.y));
						UV.y = 1.0f - UV.y;

						union PickData
						{
							uint32 UIntData[4];
							float FloatData[4];
						};

						PickData pickData;
						memcpy(pickData.UIntData, &captureContext.PickingData, sizeof(Vector4u));

						if (ImGui::BeginTooltip())
						{
							if (formatInfo.Type == FormatType::Integer)
							{
								ImGui::Text("Pos: %8d, %8d", texel.x, texel.y);
								ImGui::Text("UV: %.3f, %.3f", UV.x, UV.y);

								if (formatInfo.NumComponents == 1)
									ImGui::Text("R: %d (0x%08x)", pickData.UIntData[0], pickData.UIntData[0]);
								else if (formatInfo.NumComponents == 2)
									ImGui::Text("R: %d, G: %d (0x%08x, 0x%08x)", pickData.UIntData[0], pickData.UIntData[1], pickData.UIntData[0], pickData.UIntData[1]);
								else if (formatInfo.NumComponents == 3)
									ImGui::Text("R: %d, G: %d, B: %d (0x%08x, 0x%08x, 0x%08x)", pickData.UIntData[0], pickData.UIntData[1], pickData.UIntData[2], pickData.UIntData[0], pickData.UIntData[1], pickData.UIntData[2]);
								else if (formatInfo.NumComponents == 4)
									ImGui::Text("R: %d, G: %d, B: %d, A: %d (0x%08x, 0x%08x, 0x%08x, 0x%08x)", pickData.UIntData[0], pickData.UIntData[1], pickData.UIntData[2], pickData.UIntData[3], pickData.UIntData[0], pickData.UIntData[1], pickData.UIntData[2], pickData.UIntData[3]);
							}
							else
							{
								ImVec4 color(pickData.FloatData[0], pickData.FloatData[1], pickData.FloatData[2], pickData.FloatData[3]);
								if (formatInfo.NumComponents == 1)
									color.y = color.z = color.x;
								ImGui::ColorButton("##colorpreview", color, 0, ImVec2(64, 64));

								ImGui::SameLine();
								ImGui::BeginGroup();
								ImGui::Text("Pos: %8d, %8d", texel.x, texel.y);
								ImGui::Text("UV: %.3f, %.3f", UV.x, UV.y);

								if (formatInfo.NumComponents == 1)
									ImGui::Text("R: %.3f", pickData.FloatData[0]);
								else if (formatInfo.NumComponents == 2)
									ImGui::Text("R: %.3f, G: %.3f", pickData.FloatData[0], pickData.FloatData[1]);
								else if (formatInfo.NumComponents == 3)
									ImGui::Text("R: %.3f, G: %.3f, B: %.3f", pickData.FloatData[0], pickData.FloatData[1], pickData.FloatData[2]);
								else if (formatInfo.NumComponents == 4)
									ImGui::Text("R: %.3f, G: %.3f, B: %.3f, A: %.3f", pickData.FloatData[0], pickData.FloatData[1], pickData.FloatData[2], pickData.FloatData[3]);

								ImGui::EndGroup();
							}
						}
						ImGui::EndTooltip();
					}
				}
			}
			ImGui::PopID();
		}
		ImGui::End();
	}
}
