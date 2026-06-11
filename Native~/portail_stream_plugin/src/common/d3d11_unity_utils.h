#pragma once

#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>

namespace streamproto::d3d11
{

	inline bool AreCopyCompatibleFormats(DXGI_FORMAT lhs, DXGI_FORMAT rhs)
	{
		if (lhs == rhs)
		{
			return true;
		}

		auto format_family = [](DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return 1;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return 2;
			default:
				return 0;
			}
		};

		const int lhs_family = format_family(lhs);
		return lhs_family != 0 && lhs_family == format_family(rhs);
	}

	inline void EnableImmediateContextMultithreadProtection(ID3D11Device *device)
	{
		if (device == nullptr)
		{
			return;
		}

		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		device->GetImmediateContext(context.GetAddressOf());
		Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
		if (context != nullptr &&
			SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(multithread.GetAddressOf()))) &&
			multithread != nullptr)
		{
			multithread->SetMultithreadProtected(TRUE);
		}
	}

} // namespace streamproto::d3d11
