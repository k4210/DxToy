#pragma once

#include <vector>
#include "DXSample.h"
#include "DXSampleHelper.h"
#include "TextureResource.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class Fence
{
	HANDLE fence_event_;
	ComPtr<ID3D12Fence> fence_;
	UINT64 next_value_ = 0;

	UINT64 value_to_wait_for_ = 0;
public:
	void Initialize(ID3D12Device* device)
	{
		ThrowIfFailed(device->CreateFence(next_value_, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
		next_value_++;

		fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (fence_event_ == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	void SignalFromQueue(ID3D12CommandQueue* queue)
	{
		ThrowIfFailed(queue->Signal(fence_.Get(), next_value_));
		value_to_wait_for_ = next_value_;
		next_value_++;
	}

	void WaitForCompletion()
	{
		ThrowIfFailed(fence_->SetEventOnCompletion(value_to_wait_for_, fence_event_));
		WaitForSingleObject(fence_event_, INFINITE);
	}
};

struct LoadingRequest
{
	D3D12_SUBRESOURCE_DATA data_ = { nullptr, 0, 0 };
	CTextureResource* texture_ = nullptr;

	UINT width_ = 0;
	UINT height_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

	CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor_handle_ = CD3DX12_CPU_DESCRIPTOR_HANDLE(CD3DX12_DEFAULT{});
};

class ResourceManager
{
	ComPtr<ID3D12CommandAllocator> command_allocator_;
	ComPtr<ID3D12CommandQueue> command_queue_;
	ComPtr<ID3D12GraphicsCommandList> command_list_;

	ComPtr<ID3D12Resource> upload_heap_;

	std::vector<LoadingRequest> pending_requests;
	std::vector<LoadingRequest> processing_requests;

	Fence fence_;

	void HandleRequest(ID3D12Device* device, LoadingRequest& request)
	{
		assert(!request.texture_->resource_ && !request.texture_->upload_heap_);
		assert(EResourceState::Unloaded == request.texture_->state_);
		request.texture_->state_ = EResourceState::Loading;
		assert(device && command_list_);

		const UINT mip_levels = 1;

		CD3DX12_RESOURCE_DESC texDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			0, request.width_, request.height_, 1, static_cast<UINT16>(mip_levels), request.format_, 1, 0, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE);

		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&request.texture_->resource_)));

		{
			const UINT subresourceCount = texDesc.DepthOrArraySize * texDesc.MipLevels;
			UINT64 uploadBufferSize = GetRequiredIntermediateSize(request.texture_->resource_.Get(), 0, subresourceCount);
			ThrowIfFailed(device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&request.texture_->upload_heap_)));

			// Copy data to the intermediate upload heap and then schedule a copy
			// from the upload heap to the Texture2D.

			UpdateSubresources(command_list_.Get(), request.texture_->resource_.Get(), request.texture_->upload_heap_.Get(), 0, 0, subresourceCount, &request.data_);
			command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(request.texture_->resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		}

		// Describe and create an SRV.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = request.format_;
		srvDesc.Texture2D.MipLevels = mip_levels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		device->CreateShaderResourceView(request.texture_->resource_.Get(), &srvDesc, request.descriptor_handle_);
	}

public:
	void Initialize(ID3D12Device* device)
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;

		ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&command_queue_)));
		NAME_D3D12_OBJECT(command_queue_);

		ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&command_allocator_)));

		ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, command_allocator_.Get(), nullptr, IID_PPV_ARGS(&command_list_)));

		fence_.Initialize(device);

		//upload_heap_
	}

	void RegisterLoadRequest(LoadingRequest& request)
	{
		pending_requests.push_back(request);
	}

	void Execute(ID3D12Device* device)
	{
		assert(processing_requests.empty());
		std::swap(pending_requests, processing_requests);
		if (pending_requests.empty())
			return;

		ThrowIfFailed(command_list_->Reset(command_allocator_.Get(), nullptr));
		for(auto& it : processing_requests)
		{
			HandleRequest(device, it);
		}
		ThrowIfFailed(command_list_->Close());

		ID3D12CommandList* command_lists[] = { command_list_.Get() };
		command_queue_->ExecuteCommandLists(_countof(command_lists), command_lists);

		fence_.SignalFromQueue(command_queue_.Get());
	}

	void WaitForCopyQueue()
	{
		fence_.WaitForCompletion();
	}

	void AfterExecution()
	{
		//Assume success 
		for (auto& it : processing_requests)
		{
			assert(it.texture_ && (EResourceState::Loading == it.texture_->state_));
			it.texture_->state_ = EResourceState::Loaded;
		}
		processing_requests.clear();
	}
};
