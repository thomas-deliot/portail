#pragma once

#include <algorithm>
#include <cstdint>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "common/d3d11_unity_utils.h"

namespace streamproto::unity
{

	using Microsoft::WRL::ComPtr;

	inline bool CopySharedTextureToTarget(
		ID3D11Device *device,
		HANDLE source_handle,
		ID3D11Texture2D *target_texture,
		HANDLE &opened_source_handle,
		ComPtr<ID3D11Texture2D> &opened_source_texture,
		bool require_exact_size)
	{
		if (device == nullptr || source_handle == nullptr || target_texture == nullptr)
		{
			return false;
		}

		if (source_handle != opened_source_handle || opened_source_texture == nullptr)
		{
			opened_source_texture.Reset();
			HRESULT hr = device->OpenSharedResource(
				source_handle,
				IID_PPV_ARGS(opened_source_texture.GetAddressOf()));
			if (FAILED(hr) || opened_source_texture == nullptr)
			{
				opened_source_handle = nullptr;
				return false;
			}
			opened_source_handle = source_handle;
		}

		D3D11_TEXTURE2D_DESC src_desc{};
		D3D11_TEXTURE2D_DESC dst_desc{};
		opened_source_texture->GetDesc(&src_desc);
		target_texture->GetDesc(&dst_desc);
		if (!streamproto::d3d11::AreCopyCompatibleFormats(src_desc.Format, dst_desc.Format))
		{
			return false;
		}
		if (src_desc.Width == 0 || src_desc.Height == 0 || dst_desc.Width == 0 || dst_desc.Height == 0)
		{
			return false;
		}
		if (require_exact_size && (src_desc.Width != dst_desc.Width || src_desc.Height != dst_desc.Height))
		{
			return false;
		}

		ComPtr<ID3D11DeviceContext> context;
		device->GetImmediateContext(context.GetAddressOf());
		if (context == nullptr)
		{
			return false;
		}

		const UINT copy_width = require_exact_size ? src_desc.Width : std::min(src_desc.Width, dst_desc.Width);
		const UINT copy_height = require_exact_size ? src_desc.Height : std::min(src_desc.Height, dst_desc.Height);
		if (copy_width == 0 || copy_height == 0)
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
		context->CopySubresourceRegion(
			target_texture,
			0,
			0,
			0,
			0,
			opened_source_texture.Get(),
			0,
			&src_box);
		return true;
	}

} // namespace streamproto::unity
