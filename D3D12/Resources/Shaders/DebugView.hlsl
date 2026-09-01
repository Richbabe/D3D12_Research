#include "ImageVisualizeCommon.hlsli"

struct PassParams
{
	uint2 OutputDimensions;
	float2 UVScale;
	float2 UVBias;
	float2 ValueRange;
	uint TextureSource;
	TextureDimension TextureType;
	uint ChannelMask;
	uint MipLevel;
	uint Slice;
	uint IsIntegerFormat;
	uint ColorMap;
};

ConstantBuffer<PassParams> cPass : register(b0);
RWTexture2D<float4> uOutput : register(u0);

static const uint LegendHeight = 24;

[numthreads(8, 8, 1)]
void DebugViewCS(uint3 threadId : SV_DispatchThreadID)
{
	uint2 texel = threadId.xy;
	if(any(texel >= cPass.OutputDimensions))
		return;

	// Ramp along the bottom of the screen so a color mapped value can be read back
	if(cPass.ColorMap != ColorMap_None && texel.y >= cPass.OutputDimensions.y - LegendHeight)
	{
		float t = ((float)texel.x + 0.5f) / cPass.OutputDimensions.x;
		uOutput[texel] = float4(ApplyColorMap(cPass.ColorMap, t), 1);
		return;
	}

	float2 uv = (((float2)texel + 0.5f) / cPass.OutputDimensions) * cPass.UVScale + cPass.UVBias;
	if(any(uv < 0) || any(uv > 1))
	{
		uOutput[texel] = float4(0, 0, 0, 1);
		return;
	}

	float4 value;
	if(cPass.IsIntegerFormat)
		value = InterpretInt(SampleSource<uint4>(cPass.TextureSource, cPass.TextureType, cPass.MipLevel, cPass.Slice, uv));
	else
		value = SampleSource<float4>(cPass.TextureSource, cPass.TextureType, cPass.MipLevel, cPass.Slice, uv);

	float4 remapped = RemapChannels(value, cPass.ChannelMask, cPass.ValueRange);

	float3 color = remapped.rgb;
	if(cPass.ColorMap != ColorMap_None && countbits(cPass.ChannelMask) == 1)
		color = ApplyColorMap(cPass.ColorMap, remapped.r);

	uOutput[texel] = float4(color, 1);
}
