#pragma once

#include "Common.hlsli"
#include "Random.hlsli"
#include "ColorMaps.hlsli"

// Must match TextureType in Graphics/RHI/Texture.h
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

// Must match DebugColorMap in Graphics/Techniques/DebugView.h
static const uint ColorMap_None		= 0;
static const uint ColorMap_Viridis	= 1;
static const uint ColorMap_Plasma	= 2;
static const uint ColorMap_Magma	= 3;
static const uint ColorMap_Inferno	= 4;
static const uint ColorMap_Turbo	= 5;

// Integer values have no meaningful position on a 0-1 ramp, hash each component into a stable color instead.
float4 InterpretInt(uint4 value)
{
	uint4 seed = uint4(SeedThread(value.x), SeedThread(value.y), SeedThread(value.z), SeedThread(value.w));
	return float4(Random01(seed.x), Random01(seed.y), Random01(seed.z), Random01(seed.w));
}

template<typename T>
T SampleSource(uint sourceIndex, TextureDimension type, uint mip, uint slice, float2 uv)
{
	if(type == TextureDimension::Tex1D)
	{
		Texture1D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, uv.x, mip);
	}
	else if(type == TextureDimension::Tex2D)
	{
		Texture2D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, uv, mip);
	}
	else if(type == TextureDimension::Tex3D)
	{
		Texture3D<T> tex = ResourceDescriptorHeap[sourceIndex];
		return tex.SampleLevel(sPointClamp, float3(uv, slice), mip);
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
