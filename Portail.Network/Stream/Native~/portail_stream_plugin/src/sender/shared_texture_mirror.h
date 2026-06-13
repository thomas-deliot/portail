#pragma once

#include <algorithm>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace streamproto::sender
{

	class SharedTextureMirror
	{
	public:
		bool Init(ID3D11Device *device, ID3D11DeviceContext *context)
		{
			texture_.Reset();
			shared_handle_ = nullptr;
			width_ = 0;
			height_ = 0;
			format_ = DXGI_FORMAT_UNKNOWN;
			device_ = device;
			context_ = context;
			return device_ != nullptr && context_ != nullptr;
		}

		bool UpdateFromTexture(ID3D11Texture2D *source, int visible_width, int visible_height)
		{
			if (source == nullptr || device_ == nullptr || context_ == nullptr)
			{
				return false;
			}

			D3D11_TEXTURE2D_DESC source_desc{};
			source->GetDesc(&source_desc);
			const UINT copy_width = std::clamp<UINT>(
				static_cast<UINT>(std::max(visible_width, 1)),
				1,
				source_desc.Width);
			const UINT copy_height = std::clamp<UINT>(
				static_cast<UINT>(std::max(visible_height, 1)),
				1,
				source_desc.Height);
			if (!EnsureTexture(source_desc, static_cast<int>(copy_width), static_cast<int>(copy_height)))
			{
				return false;
			}

			D3D11_BOX src_box{};
			src_box.left = 0;
			src_box.top = 0;
			src_box.front = 0;
			src_box.right = copy_width;
			src_box.bottom = copy_height;
			src_box.back = 1;
			context_->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0, source, 0, &src_box);
			context_->Flush();
			return true;
		}

		[[nodiscard]] HANDLE SharedHandle() const { return shared_handle_; }
		[[nodiscard]] ID3D11Texture2D *Texture() const { return texture_.Get(); }
		[[nodiscard]] int Width() const { return width_; }
		[[nodiscard]] int Height() const { return height_; }
		[[nodiscard]] DXGI_FORMAT Format() const { return format_; }

	private:
		bool EnsureTexture(const D3D11_TEXTURE2D_DESC &source_desc, int width, int height)
		{
			if (texture_ != nullptr &&
				width_ == width &&
				height_ == height &&
				format_ == source_desc.Format)
			{
				return true;
			}

			texture_.Reset();
			shared_handle_ = nullptr;

			D3D11_TEXTURE2D_DESC out_desc{};
			out_desc.Width = static_cast<UINT>(std::max(width, 1));
			out_desc.Height = static_cast<UINT>(std::max(height, 1));
			out_desc.MipLevels = 1;
			out_desc.ArraySize = 1;
			out_desc.Format = source_desc.Format;
			out_desc.SampleDesc.Count = 1;
			out_desc.Usage = D3D11_USAGE_DEFAULT;
			out_desc.CPUAccessFlags = 0;

			UINT bind_flags = source_desc.BindFlags;
			if ((bind_flags & D3D11_BIND_SHADER_RESOURCE) == 0)
			{
				bind_flags |= D3D11_BIND_SHADER_RESOURCE;
			}
			out_desc.BindFlags = bind_flags;
			out_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
			HRESULT hr = device_->CreateTexture2D(&out_desc, nullptr, texture_.GetAddressOf());
			if (FAILED(hr) || texture_ == nullptr)
			{
				return false;
			}

			Microsoft::WRL::ComPtr<IDXGIResource> resource;
			if (FAILED(texture_.As(&resource)) || resource == nullptr)
			{
				texture_.Reset();
				return false;
			}
			if (FAILED(resource->GetSharedHandle(&shared_handle_)))
			{
				texture_.Reset();
				shared_handle_ = nullptr;
				return false;
			}

			width_ = width;
			height_ = height;
			format_ = source_desc.Format;
			return true;
		}

		Microsoft::WRL::ComPtr<ID3D11Device> device_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
		HANDLE shared_handle_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	};

} // namespace streamproto::sender
