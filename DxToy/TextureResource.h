#pragma once

#include "DXSample.h"
#include "DXSampleHelper.h"

enum class EResourceState : UINT
{
	Unloaded,
	Loading,
	Loaded,
	Unloading,
};

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class CTextureResource
{
public:
	ComPtr<ID3D12Resource> resource_;
	ComPtr<ID3D12Resource> upload_heap_;

	EResourceState state_ = EResourceState::Unloaded;

	void StartLoad(ComPtr<ID3D12Device> device, ComPtr<ID3D12GraphicsCommandList> command_list
		, UINT width, UINT height, DXGI_FORMAT format
		, D3D12_SUBRESOURCE_DATA data
		, CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor_handle)
	{
		assert(!resource_ && !upload_heap_);
		assert(EResourceState::Unloaded == state_);
		state_ = EResourceState::Loading;
		assert(device && command_list);

		const UINT mip_levels = 1;

		CD3DX12_RESOURCE_DESC texDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			0, width, height, 1, static_cast<UINT16>(mip_levels), format, 1, 0, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE);

		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&resource_)));

		{
			const UINT subresourceCount = texDesc.DepthOrArraySize * texDesc.MipLevels;
			UINT64 uploadBufferSize = GetRequiredIntermediateSize(resource_.Get(), 0, subresourceCount);
			ThrowIfFailed(device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&upload_heap_)));

			// Copy data to the intermediate upload heap and then schedule a copy
			// from the upload heap to the Texture2D.

			UpdateSubresources(command_list.Get(), resource_.Get(), upload_heap_.Get(), 0, 0, subresourceCount, &data);
			command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		}

		// Describe and create an SRV.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = format;
		srvDesc.Texture2D.MipLevels = mip_levels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		device->CreateShaderResourceView(resource_.Get(), &srvDesc, descriptor_handle);
	}

	void EndLoad()
	{
		state_ = EResourceState::Loaded;
	}

	void StartUnload(ComPtr<ID3D12Device> device, CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor_handle)
	{
		state_ = EResourceState::Unloading;

		D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
		nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		nullSrvDesc.Texture2D.MipLevels = 1;
		nullSrvDesc.Texture2D.MostDetailedMip = 0;
		nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		device->CreateShaderResourceView(nullptr, &nullSrvDesc, descriptor_handle);
	}

	void EndUnload()
	{
		state_ = EResourceState::Unloaded;
		upload_heap_ = nullptr;
		resource_ = nullptr;
	}

	bool IsValid() const { return EResourceState::Loaded == state_; }
};