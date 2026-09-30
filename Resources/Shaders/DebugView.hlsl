#include "Common.hlsli"
#include "Random.hlsli"
#include "ColorMaps.hlsli"

// Must match TextureType in RHI/Texture.h
enum TextureDimension : uint
{
	Tex1D,
	Tex1DArray,
	Tex2D,
	Tex2DArray,
	Tex3D,
	TexCube,
	TexCubeArray,
};

// Must match DebugColorMap in Renderer/Techniques/DebugView.h
static const uint ColorMap_None		= 0;
static const uint ColorMap_Viridis	= 1;
static const uint ColorMap_Plasma	= 2;
static const uint ColorMap_Magma	= 3;
static const uint ColorMap_Inferno	= 4;
static const uint ColorMap_Turbo	= 5;

struct PassParams
{
	uint2 OutputDimensions;
	float2 UVScale;
	float2 UVBias;
	float2 ValueRange;
	DescriptorHandleBase TextureSource;
	RWTexture2DH<float4> Output;
	TextureDimension TextureType;
	uint ChannelMask;
	uint MipLevel;
	uint Slice;
	uint IsIntegerFormat;
	uint ColorMap;
};
DEFINE_CONSTANTS(PassParams, 0);

static const uint LegendHeight = 24;

// Integer values have no meaningful position on a 0-1 ramp, hash each component into a stable color instead.
float4 InterpretInt(uint4 value)
{
	uint4 seed = uint4(SeedThread(value.x), SeedThread(value.y), SeedThread(value.z), SeedThread(value.w));
	return float4(Random01(seed.x), Random01(seed.y), Random01(seed.z), Random01(seed.w));
}

template<typename T>
T SampleSource(float2 uv)
{
	PassParams passParams = cPassParams;
	uint sourceIndex = passParams.TextureSource.GetIndex();
	uint mip = passParams.MipLevel;

	if(passParams.TextureType == TextureDimension::Tex1D)
	{
		Texture1D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, uv.x, mip);
	}
	else if(passParams.TextureType == TextureDimension::Tex2D)
	{
		Texture2D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, uv, mip);
	}
	else if(passParams.TextureType == TextureDimension::Tex3D)
	{
		Texture3D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, float3(uv, passParams.Slice), mip);
	}
	// Array and cube dimensions are unsupported, output magenta so it's obvious
	return T(1, 0, 1, 1);
}

// A single visible channel is remapped to greyscale, otherwise each visible channel is remapped in place.
float4 RemapChannels(float4 value, uint channelMask, float2 range)
{
	float4 output = 0;
	if(countbits(channelMask) == 1)
	{
		uint singleChannel = firstbithigh(channelMask);
		output = float4(InverseLerp(value[singleChannel], range.x, range.y).xxx, 1);
	}
	else
	{
		output.r = ((channelMask >> 0) & 1) * InverseLerp(value[0], range.x, range.y);
		output.g = ((channelMask >> 1) & 1) * InverseLerp(value[1], range.x, range.y);
		output.b = ((channelMask >> 2) & 1) * InverseLerp(value[2], range.x, range.y);
		output.a = (channelMask >> 3) & 1 ? value.a : 1.0f;
	}
	return output;
}

float3 ApplyColorMap(uint colorMap, float t)
{
	t = saturate(t);
	switch(colorMap)
	{
	case ColorMap_Viridis:	return Viridis(t);
	case ColorMap_Plasma:	return Plasma(t);
	case ColorMap_Magma:	return Magma(t);
	case ColorMap_Inferno:	return Inferno(t);
	case ColorMap_Turbo:	return Turbo(t);
	}
	return t.xxx;
}

[numthreads(8, 8, 1)]
void DebugViewCS(uint3 threadId : SV_DispatchThreadID)
{
	PassParams passParams = cPassParams;
	uint2 texel = threadId.xy;
	if(any(texel >= passParams.OutputDimensions))
		return;

	// Ramp along the bottom of the screen so a color mapped value can be read back
	if(passParams.ColorMap != ColorMap_None && texel.y >= passParams.OutputDimensions.y - LegendHeight)
	{
		float t = ((float)texel.x + 0.5f) / passParams.OutputDimensions.x;
		passParams.Output.Store(texel, float4(ApplyColorMap(passParams.ColorMap, t), 1));
		return;
	}

	float2 uv = (((float2)texel + 0.5f) / passParams.OutputDimensions) * passParams.UVScale + passParams.UVBias;
	if(any(uv < 0) || any(uv > 1))
	{
		passParams.Output.Store(texel, float4(0, 0, 0, 1));
		return;
	}

	float4 value;
	if(passParams.IsIntegerFormat)
		value = InterpretInt(SampleSource<uint4>(uv));
	else
		value = SampleSource<float4>(uv);

	float4 remapped = RemapChannels(value, passParams.ChannelMask, passParams.ValueRange);

	float3 color = remapped.rgb;
	if(passParams.ColorMap != ColorMap_None && countbits(passParams.ChannelMask) == 1)
		color = ApplyColorMap(passParams.ColorMap, remapped.r);

	passParams.Output.Store(texel, float4(color, 1));
}
