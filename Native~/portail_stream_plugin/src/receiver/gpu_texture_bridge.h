#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

namespace streamproto::receiver
{

	using Microsoft::WRL::ComPtr;

	class GpuTextureBridge
	{
	public:
		GpuTextureBridge() = default;
		~GpuTextureBridge() { Shutdown(); }

		bool Init(AVBufferRef *hw_device_ctx, std::string &error)
		{
			ComPtr<ID3D11Device> next_device;
			ComPtr<ID3D11DeviceContext> next_context;
			if (!ExtractD3D11Context(hw_device_ctx, next_device, next_context, error))
			{
				Shutdown();
				return false;
			}

			if (device_.Get() == next_device.Get() &&
				context_.Get() == next_context.Get() &&
				video_device_ != nullptr &&
				video_context_ != nullptr)
			{
				return true;
			}

			Shutdown();
			device_ = next_device;
			context_ = next_context;

			ComPtr<ID3D11Multithread> multithread;
			if (context_ != nullptr && SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(multithread.GetAddressOf()))) && multithread != nullptr)
			{
				multithread->SetMultithreadProtected(TRUE);
			}

			if (FAILED(device_.As(&video_device_)) || video_device_ == nullptr)
			{
				error = "Failed to acquire ID3D11VideoDevice from decoder device.";
				Shutdown();
				return false;
			}
			if (FAILED(context_.As(&video_context_)) || video_context_ == nullptr)
			{
				error = "Failed to acquire ID3D11VideoContext from decoder context.";
				Shutdown();
				return false;
			}
			return true;
		}

		bool UpdateFromFrame(const AVFrame *frame)
		{
			if (frame == nullptr || frame->format != AV_PIX_FMT_D3D11 || device_ == nullptr || context_ == nullptr)
			{
				return false;
			}

			auto *source_texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
			if (source_texture == nullptr)
			{
				return false;
			}
			const UINT source_subresource = static_cast<UINT>(reinterpret_cast<std::uintptr_t>(frame->data[1]));

			D3D11_TEXTURE2D_DESC source_desc{};
			source_texture->GetDesc(&source_desc);
			const UINT visible_width = std::clamp<UINT>(
				static_cast<UINT>(std::max(frame->width, 1)),
				1,
				source_desc.Width);
			const UINT visible_height = std::clamp<UINT>(
				static_cast<UINT>(std::max(frame->height, 1)),
				1,
				source_desc.Height);

			const DXGI_FORMAT output_format = NormalizeOutputFormat(source_desc.Format);
			const bool use_video_processor = source_desc.Format != output_format || !IsDisplayableColorFormat(source_desc.Format);
			if (!EnsureOutputTexture(
					source_desc,
					static_cast<int>(visible_width),
					static_cast<int>(visible_height),
					output_format,
					use_video_processor))
			{
				return false;
			}

			if (!use_video_processor)
			{
				D3D11_BOX src_box{};
				src_box.left = 0;
				src_box.top = 0;
				src_box.front = 0;
				src_box.right = visible_width;
				src_box.bottom = visible_height;
				src_box.back = 1;

				context_->CopySubresourceRegion(
					texture_.Get(),
					0,
					0,
					0,
					0,
					source_texture,
					source_subresource,
					&src_box);
				++frames_copied_;
				return true;
			}

			if (!EnsureVideoProcessor(source_desc, static_cast<int>(visible_width), static_cast<int>(visible_height)))
			{
				return false;
			}
			if (!EnsureOutputView())
			{
				return false;
			}

			ComPtr<ID3D11VideoProcessorInputView> input_view;
			if (!GetOrCreateInputView(source_texture, source_subresource, source_desc, input_view))
			{
				return false;
			}

			RECT src_rect{
				0,
				0,
				static_cast<LONG>(visible_width),
				static_cast<LONG>(visible_height)};
			RECT dst_rect{
				0,
				0,
				static_cast<LONG>(std::max(width_, 1)),
				static_cast<LONG>(std::max(height_, 1))};

			video_context_->VideoProcessorSetStreamSourceRect(video_proc_.Get(), 0, TRUE, &src_rect);
			video_context_->VideoProcessorSetStreamDestRect(video_proc_.Get(), 0, TRUE, &dst_rect);
			video_context_->VideoProcessorSetOutputTargetRect(video_proc_.Get(), TRUE, &dst_rect);
			video_context_->VideoProcessorSetStreamFrameFormat(video_proc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
			const D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color_space = BuildInputColorSpace(frame);
			const D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color_space = BuildFullRangeBt709ColorSpace();
			video_context_->VideoProcessorSetStreamColorSpace(video_proc_.Get(), 0, &input_color_space);
			video_context_->VideoProcessorSetOutputColorSpace(video_proc_.Get(), &output_color_space);
			video_context_->VideoProcessorSetStreamAutoProcessingMode(video_proc_.Get(), 0, FALSE);
			video_context_->VideoProcessorSetStreamAlpha(video_proc_.Get(), 0, TRUE, 1.0f);
			video_context_->VideoProcessorSetOutputAlphaFillMode(
				video_proc_.Get(),
				D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE,
				0);

			D3D11_VIDEO_PROCESSOR_STREAM stream{};
			stream.Enable = TRUE;
			stream.pInputSurface = input_view.Get();

			if (FAILED(video_context_->VideoProcessorBlt(video_proc_.Get(), output_view_.Get(), 0, 1, &stream)))
			{
				return false;
			}

			++frames_copied_;
			return true;
		}

		void Shutdown()
		{
			video_proc_.Reset();
			vp_enum_.Reset();
			input_view_cache_.clear();
			output_view_.Reset();
			video_context_.Reset();
			video_device_.Reset();
			texture_.Reset();
			context_.Reset();
			device_.Reset();
			shared_handle_ = nullptr;
			width_ = 0;
			height_ = 0;
			format_ = DXGI_FORMAT_UNKNOWN;
			uses_video_processor_ = false;
			vp_input_width_ = 0;
			vp_input_height_ = 0;
			vp_output_width_ = 0;
			vp_output_height_ = 0;
			vp_input_format_ = DXGI_FORMAT_UNKNOWN;
			frames_copied_ = 0;
		}

		[[nodiscard]] bool Ready() const { return texture_ != nullptr; }
		[[nodiscard]] int Width() const { return width_; }
		[[nodiscard]] int Height() const { return height_; }
		[[nodiscard]] DXGI_FORMAT Format() const { return format_; }
		[[nodiscard]] std::uint64_t SharedHandle() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(shared_handle_)); }
		[[nodiscard]] std::uint64_t TexturePtr() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture_.Get())); }
		[[nodiscard]] ID3D11Texture2D *Texture() const { return texture_.Get(); }
		[[nodiscard]] ID3D11Device *Device() const { return device_.Get(); }
		[[nodiscard]] ID3D11DeviceContext *Context() const { return context_.Get(); }

	private:
		struct CachedInputView
		{
			ID3D11Texture2D *texture = nullptr;
			UINT subresource = 0;
			ComPtr<ID3D11VideoProcessorInputView> view;
		};

		static bool ExtractD3D11Context(
			AVBufferRef *hw_device_ctx,
			ComPtr<ID3D11Device> &out_device,
			ComPtr<ID3D11DeviceContext> &out_context,
			std::string &error)
		{
			out_device.Reset();
			out_context.Reset();
			if (hw_device_ctx == nullptr)
			{
				error = "Decoder has no hardware device context.";
				return false;
			}

			auto *dev = reinterpret_cast<AVHWDeviceContext *>(hw_device_ctx->data);
			if (dev == nullptr || dev->type != AV_HWDEVICE_TYPE_D3D11VA || dev->hwctx == nullptr)
			{
				error = "Hardware device context is not D3D11VA.";
				return false;
			}

			auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(dev->hwctx);
			if (d3d->device == nullptr)
			{
				error = "D3D11 device is null in hwctx.";
				return false;
			}

			out_device = d3d->device;
			if (d3d->device_context != nullptr)
			{
				out_context = d3d->device_context;
			}
			else
			{
				out_device->GetImmediateContext(out_context.GetAddressOf());
			}

			if (out_device == nullptr || out_context == nullptr)
			{
				error = "Failed to acquire D3D11 device/context from decoder.";
				out_device.Reset();
				out_context.Reset();
				return false;
			}
			return true;
		}

		static bool IsDisplayableColorFormat(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return true;
			default:
				return false;
			}
		}

		static DXGI_FORMAT NormalizeOutputFormat(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			default:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			}
		}

		static D3D11_VIDEO_PROCESSOR_COLOR_SPACE BuildFullRangeBt709ColorSpace()
		{
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space{};
			color_space.Usage = 0;
			color_space.RGB_Range = 0;
			color_space.YCbCr_Matrix = 1;
			color_space.YCbCr_xvYCC = 0;
			color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
			return color_space;
		}

		static D3D11_VIDEO_PROCESSOR_COLOR_SPACE BuildInputColorSpace(const AVFrame *frame)
		{
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space = BuildFullRangeBt709ColorSpace();
			if (frame != nullptr && frame->color_range != AVCOL_RANGE_JPEG)
			{
				color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
			}
			return color_space;
		}

		bool EnsureOutputTexture(
			const D3D11_TEXTURE2D_DESC &source_desc,
			int visible_width,
			int visible_height,
			DXGI_FORMAT output_format,
			bool use_video_processor)
		{
			if (texture_ != nullptr &&
				width_ == visible_width &&
				height_ == visible_height &&
				format_ == output_format &&
				uses_video_processor_ == use_video_processor)
			{
				return true;
			}

			texture_.Reset();
			output_view_.Reset();
			input_view_cache_.clear();
			shared_handle_ = nullptr;

			D3D11_TEXTURE2D_DESC out_desc{};
			out_desc.Width = static_cast<UINT>(std::max(visible_width, 1));
			out_desc.Height = static_cast<UINT>(std::max(visible_height, 1));
			out_desc.MipLevels = 1;
			out_desc.ArraySize = 1;
			out_desc.Format = output_format;
			out_desc.SampleDesc.Count = 1;
			out_desc.Usage = D3D11_USAGE_DEFAULT;
			out_desc.CPUAccessFlags = 0;

			UINT preferred_bind_flags = D3D11_BIND_SHADER_RESOURCE;
			if (use_video_processor)
			{
				preferred_bind_flags |= D3D11_BIND_RENDER_TARGET;
			}
			else if ((source_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
			{
				preferred_bind_flags = source_desc.BindFlags;
			}

			preferred_bind_flags &= ~(D3D11_BIND_DECODER | D3D11_BIND_VIDEO_ENCODER);
			if (preferred_bind_flags == 0)
			{
				preferred_bind_flags = D3D11_BIND_SHADER_RESOURCE;
				if (use_video_processor)
				{
					preferred_bind_flags |= D3D11_BIND_RENDER_TARGET;
				}
			}

			auto try_create = [&](UINT bind_flags, UINT misc_flags)
			{
				texture_.Reset();
				shared_handle_ = nullptr;
				out_desc.BindFlags = bind_flags;
				out_desc.MiscFlags = misc_flags;
				HRESULT create_hr = device_->CreateTexture2D(&out_desc, nullptr, texture_.GetAddressOf());
				if (FAILED(create_hr) || texture_ == nullptr)
				{
					texture_.Reset();
					return false;
				}
				if ((misc_flags & D3D11_RESOURCE_MISC_SHARED) != 0)
				{
					ComPtr<IDXGIResource> resource;
					if (SUCCEEDED(texture_.As(&resource)) && resource != nullptr)
					{
						resource->GetSharedHandle(&shared_handle_);
					}
				}
				return true;
			};

			bool created = false;
			created = created || try_create(preferred_bind_flags, D3D11_RESOURCE_MISC_SHARED);
			if (!created)
			{
				created = created || try_create(preferred_bind_flags, 0);
			}
			if (!created)
			{
				return false;
			}

			width_ = static_cast<int>(out_desc.Width);
			height_ = static_cast<int>(out_desc.Height);
			format_ = output_format;
			uses_video_processor_ = use_video_processor;
			return true;
		}

		bool EnsureOutputView()
		{
			if (!uses_video_processor_ || texture_ == nullptr || video_device_ == nullptr || vp_enum_ == nullptr)
			{
				return false;
			}
			if (output_view_ != nullptr)
			{
				return true;
			}

			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
			out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
			out_desc.Texture2D.MipSlice = 0;
			return SUCCEEDED(video_device_->CreateVideoProcessorOutputView(
					   texture_.Get(),
					   vp_enum_.Get(),
					   &out_desc,
					   output_view_.GetAddressOf())) &&
				   output_view_ != nullptr;
		}

		bool GetOrCreateInputView(
			ID3D11Texture2D *texture,
			UINT source_subresource,
			const D3D11_TEXTURE2D_DESC &source_desc,
			ComPtr<ID3D11VideoProcessorInputView> &out_view)
		{
			out_view.Reset();
			if (texture == nullptr || video_device_ == nullptr || vp_enum_ == nullptr)
			{
				return false;
			}

			const UINT mip_levels = std::max<UINT>(1, source_desc.MipLevels);
			const UINT mip_slice = source_subresource % mip_levels;
			const UINT array_slice = source_subresource / mip_levels;
			if (array_slice >= std::max<UINT>(1, source_desc.ArraySize))
			{
				return false;
			}

			for (auto &entry : input_view_cache_)
			{
				if (entry.texture == texture && entry.subresource == source_subresource && entry.view != nullptr)
				{
					out_view = entry.view;
					return true;
				}
			}

			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
			in_desc.FourCC = 0;
			in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			in_desc.Texture2D.MipSlice = mip_slice;
			in_desc.Texture2D.ArraySlice = array_slice;

			ComPtr<ID3D11VideoProcessorInputView> input_view;
			if (FAILED(video_device_->CreateVideoProcessorInputView(texture, vp_enum_.Get(), &in_desc, input_view.GetAddressOf())) ||
				input_view == nullptr)
			{
				return false;
			}

			input_view_cache_.push_back(CachedInputView{texture, source_subresource, input_view});
			if (input_view_cache_.size() > 16)
			{
				input_view_cache_.pop_front();
			}

			out_view = input_view;
			return true;
		}

		bool EnsureVideoProcessor(const D3D11_TEXTURE2D_DESC &source_desc, int output_width, int output_height)
		{
			if (video_device_ == nullptr || video_context_ == nullptr)
			{
				return false;
			}

			output_width = std::max(output_width, 1);
			output_height = std::max(output_height, 1);
			if (vp_enum_ != nullptr &&
				video_proc_ != nullptr &&
				vp_input_width_ == static_cast<int>(source_desc.Width) &&
				vp_input_height_ == static_cast<int>(source_desc.Height) &&
				vp_input_format_ == source_desc.Format &&
				vp_output_width_ == output_width &&
				vp_output_height_ == output_height)
			{
				return true;
			}

			video_proc_.Reset();
			vp_enum_.Reset();
			input_view_cache_.clear();
			output_view_.Reset();

			D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
			content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
			content.InputWidth = source_desc.Width;
			content.InputHeight = source_desc.Height;
			content.OutputWidth = static_cast<UINT>(output_width);
			content.OutputHeight = static_cast<UINT>(output_height);
			content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

			if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content, vp_enum_.GetAddressOf())) || vp_enum_ == nullptr)
			{
				return false;
			}
			if (FAILED(video_device_->CreateVideoProcessor(vp_enum_.Get(), 0, video_proc_.GetAddressOf())) || video_proc_ == nullptr)
			{
				return false;
			}

			vp_input_width_ = static_cast<int>(source_desc.Width);
			vp_input_height_ = static_cast<int>(source_desc.Height);
			vp_input_format_ = source_desc.Format;
			vp_output_width_ = output_width;
			vp_output_height_ = output_height;
			return true;
		}

		ComPtr<ID3D11Device> device_;
		ComPtr<ID3D11DeviceContext> context_;
		ComPtr<ID3D11VideoDevice> video_device_;
		ComPtr<ID3D11VideoContext> video_context_;
		ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
		ComPtr<ID3D11VideoProcessor> video_proc_;
		ComPtr<ID3D11VideoProcessorOutputView> output_view_;
		std::deque<CachedInputView> input_view_cache_;
		ComPtr<ID3D11Texture2D> texture_;
		HANDLE shared_handle_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
		bool uses_video_processor_ = false;
		int vp_input_width_ = 0;
		int vp_input_height_ = 0;
		int vp_output_width_ = 0;
		int vp_output_height_ = 0;
		DXGI_FORMAT vp_input_format_ = DXGI_FORMAT_UNKNOWN;
		std::uint64_t frames_copied_ = 0;
	};

} // namespace streamproto::receiver
