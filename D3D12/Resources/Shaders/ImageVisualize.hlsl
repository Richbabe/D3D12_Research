#include "ImageVisualizeCommon.hlsli"

struct PickingData
{
	float4 DataFloat;
	uint4  DataUInt;
};

struct ConstantsData
{
	uint2 HoveredPixel;
    uint2 Dimensions;
	float2 ValueRange;
	uint TextureSource;
    uint TextureTarget;
	TextureDimension TextureType;
	uint ChannelMask;
	uint MipLevel;
	uint Slice;
	uint IsIntegerFormat;
};

ConstantBuffer<ConstantsData> cConstants : register(b0);
RWStructuredBuffer<PickingData> uPickingData : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
	uint2 texel = threadId.xy;
	if(any(texel >= cConstants.Dimensions))
		return;

	PickingData data;
	float4 value;
	float2 uv = (texel + 0.5f) / cConstants.Dimensions;
	if(cConstants.IsIntegerFormat)
	{
		data.DataUInt = SampleSource<uint4>(cConstants.TextureSource, cConstants.TextureType, cConstants.MipLevel, cConstants.Slice, uv);
		value = InterpretInt(data.DataUInt);
	}
	else
	{
		data.DataFloat = SampleSource<float4>(cConstants.TextureSource, cConstants.TextureType, cConstants.MipLevel, cConstants.Slice, uv);
		value = data.DataFloat;
	}

	if(all(texel == cConstants.HoveredPixel))
		uPickingData[0] = data;

	float4 output = RemapChannels(value, cConstants.ChannelMask, cConstants.ValueRange);

	RWTexture2D<float4> target = ResourceDescriptorHeap[cConstants.TextureTarget];
	target[texel] = output;
}
