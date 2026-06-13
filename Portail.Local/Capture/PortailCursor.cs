using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Portail.Core;
using UnityEngine;

namespace Portail.Capture
{
	[DefaultExecutionOrder(10000)]
	[DisallowMultipleComponent]
	public sealed class PortailCursor : MonoBehaviour
	{
		[Header("Activation")]
		[SerializeField] bool onlyWhilePortailControl = true;

		[Header("Real Cursor")]
		[SerializeField] bool hideRealCursor = true;
		[SerializeField] bool replaceSystemCursorsWithTransparent = true;

		[Header("Compositing")]
		[SerializeField] bool drawIntoCaptureTexture = true;
		[SerializeField] Shader cursorCompositeShader;
		[SerializeField, Min(16)] int maxCursorTextureSize = 512;

		const int CursorShowing = 0x00000001;
		const int DibRgbColors = 0;
		const int BiRgb = 0;
		const uint DiNormal = 0x0003;
		const uint SpiSetCursors = 0x0057;
		const int SmCxCursor = 13;
		const int SmCyCursor = 14;

		static readonly uint[] SystemCursorIds =
		{
			32512, // OCR_NORMAL
			32513, // OCR_IBEAM
			32514, // OCR_WAIT
			32515, // OCR_CROSS
			32516, // OCR_UP
			32642, // OCR_SIZENWSE
			32643, // OCR_SIZENESW
			32644, // OCR_SIZEWE
			32645, // OCR_SIZENS
			32646, // OCR_SIZEALL
			32648, // OCR_NO
			32649, // OCR_HAND
			32650, // OCR_APPSTARTING
			32651, // OCR_HELP
			32671, // OCR_PIN
			32672, // OCR_PERSON
		};

		readonly Dictionary<IntPtr, CursorFrame> _cursorFramesByHandle = new Dictionary<IntPtr, CursorFrame>();
		readonly Dictionary<IntPtr, CursorFrame> _hiddenSystemCursorFrames = new Dictionary<IntPtr, CursorFrame>();

		Material _cursorMaterial;
		bool _systemCursorsHidden;
		bool _wasActive;
		bool _hasSnapshot;
		CursorSnapshot _snapshot;

		sealed class CursorFrame
		{
			public Texture2D Texture;
			public Vector2Int Hotspot;
			public Vector2Int Size;
		}

		struct CursorSnapshot
		{
			public CursorFrame Frame;
			public POINT ScreenPosition;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct POINT
		{
			public int x;
			public int y;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct CURSORINFO
		{
			public int cbSize;
			public int flags;
			public IntPtr hCursor;
			public POINT ptScreenPos;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct ICONINFO
		{
			[MarshalAs(UnmanagedType.Bool)] public bool fIcon;
			public int xHotspot;
			public int yHotspot;
			public IntPtr hbmMask;
			public IntPtr hbmColor;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct BITMAP
		{
			public int bmType;
			public int bmWidth;
			public int bmHeight;
			public int bmWidthBytes;
			public ushort bmPlanes;
			public ushort bmBitsPixel;
			public IntPtr bmBits;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct BITMAPINFO
		{
			public BITMAPINFOHEADER bmiHeader;
			public RGBQUAD bmiColors;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct BITMAPINFOHEADER
		{
			public uint biSize;
			public int biWidth;
			public int biHeight;
			public ushort biPlanes;
			public ushort biBitCount;
			public uint biCompression;
			public uint biSizeImage;
			public int biXPelsPerMeter;
			public int biYPelsPerMeter;
			public uint biClrUsed;
			public uint biClrImportant;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct RGBQUAD
		{
			public byte rgbBlue;
			public byte rgbGreen;
			public byte rgbRed;
			public byte rgbReserved;
		}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool GetCursorInfo(ref CURSORINFO pci);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool GetIconInfo(IntPtr hIcon, out ICONINFO piconinfo);

		[DllImport("user32.dll")]
		static extern IntPtr SetCursor(IntPtr hCursor);

		[DllImport("user32.dll", SetLastError = true)]
		static extern IntPtr CreateCursor(
			IntPtr hInst,
			int xHotSpot,
			int yHotSpot,
			int nWidth,
			int nHeight,
			byte[] pvANDPlane,
			byte[] pvXORPlane);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool SetSystemCursor(IntPtr hcur, uint id);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool DestroyCursor(IntPtr hCursor);

		[DllImport("user32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool SystemParametersInfo(uint uiAction, uint uiParam, IntPtr pvParam, uint fWinIni);

		[DllImport("user32.dll", SetLastError = true)]
		static extern IntPtr LoadCursor(IntPtr hInstance, IntPtr lpCursorName);

		[DllImport("user32.dll")]
		static extern int GetSystemMetrics(int nIndex);

		[DllImport("user32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool DrawIconEx(
			IntPtr hdc,
			int xLeft,
			int yTop,
			IntPtr hIcon,
			int cxWidth,
			int cyWidth,
			uint istepIfAniCur,
			IntPtr hbrFlickerFreeDraw,
			uint diFlags);

		[DllImport("gdi32.dll")]
		static extern int GetObject(IntPtr hgdiobj, int cbBuffer, out BITMAP lpvObject);

		[DllImport("gdi32.dll")]
		static extern IntPtr CreateCompatibleDC(IntPtr hdc);

		[DllImport("gdi32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool DeleteDC(IntPtr hdc);

		[DllImport("gdi32.dll")]
		static extern IntPtr SelectObject(IntPtr hdc, IntPtr hgdiobj);

		[DllImport("gdi32.dll")]
		[return: MarshalAs(UnmanagedType.Bool)]
		static extern bool DeleteObject(IntPtr hObject);

		[DllImport("gdi32.dll")]
		static extern IntPtr CreateDIBSection(
			IntPtr hdc,
			ref BITMAPINFO pbmi,
			uint usage,
			out IntPtr ppvBits,
			IntPtr hSection,
			uint offset);
#endif

		void OnDisable()
		{
			RestoreRealCursorIfNeeded(_wasActive);
		}

		void OnDestroy()
		{
			RestoreSystemCursors();
			ReleaseCursorFrames(_cursorFramesByHandle);
			ReleaseCursorFrames(_hiddenSystemCursorFrames);

			if (_cursorMaterial != null)
			{
				Destroy(_cursorMaterial);
				_cursorMaterial = null;
			}
		}

		void OnApplicationQuit()
		{
			RestoreSystemCursors();
		}

		void Update()
		{
			bool active = ShouldControlCursor();

			if (!active)
			{
				_hasSnapshot = false;
				if (_wasActive)
				{
					RestoreRealCursorIfNeeded(false);
				}
				_wasActive = false;
				return;
			}

			_wasActive = true;
			HideRealCursorIfNeeded();
		}

		void LateUpdate()
		{
			if (ShouldControlCursor())
			{
				HideRealCursorIfNeeded();
				SampleCursor();
				CompositeCursorIntoCaptureTexture();
			}
		}

		bool ShouldControlCursor()
		{
			return !onlyWhilePortailControl ||
				PortailControlState.IsDesktopControlActive;
		}

		void HideRealCursorIfNeeded()
		{
			if (!hideRealCursor)
			{
				RestoreSystemCursors();
				return;
			}

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (replaceSystemCursorsWithTransparent)
			{
				EnsureSystemCursorsHidden();
			}

			Cursor.visible = false;
			SetCursor(IntPtr.Zero);
#endif
		}

		void RestoreRealCursorIfNeeded(bool restoreUnityCursorVisible)
		{
			RestoreSystemCursors();
			_hasSnapshot = false;
			if (restoreUnityCursorVisible)
			{
				Cursor.visible = true;
			}
		}

		void SampleCursor()
		{
			_hasSnapshot = false;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			CURSORINFO cursorInfo = new CURSORINFO
			{
				cbSize = Marshal.SizeOf<CURSORINFO>(),
			};

			if (!GetCursorInfo(ref cursorInfo) ||
				(cursorInfo.flags & CursorShowing) == 0 ||
				cursorInfo.hCursor == IntPtr.Zero)
			{
				return;
			}

			if (!TryGetCursorFrame(cursorInfo.hCursor, out CursorFrame frame))
			{
				return;
			}

			_snapshot = new CursorSnapshot
			{
				Frame = frame,
				ScreenPosition = cursorInfo.ptScreenPos,
			};
			_hasSnapshot = true;
#endif
		}

		bool TryGetCursorFrame(IntPtr cursorHandle, out CursorFrame frame)
		{
			frame = null;
			if (cursorHandle == IntPtr.Zero)
			{
				return false;
			}

			if (_hiddenSystemCursorFrames.TryGetValue(cursorHandle, out frame) && IsValidFrame(frame))
			{
				return true;
			}

			if (_cursorFramesByHandle.TryGetValue(cursorHandle, out frame) && IsValidFrame(frame))
			{
				return true;
			}

			if (!TryCaptureCursorFrame(cursorHandle, out frame))
			{
				return false;
			}

			_cursorFramesByHandle[cursorHandle] = frame;
			return true;
		}

		void CompositeCursorIntoCaptureTexture()
		{
			if (!_hasSnapshot || !drawIntoCaptureTexture)
			{
				return;
			}

			PortailCaptureSession manager = PortailCaptureSession.Instance;
			RenderTexture target = manager != null ? manager.CaptureTexture as RenderTexture : null;
			CursorFrame frame = _snapshot.Frame;
			if (target == null || !IsValidFrame(frame) ||
				!manager.TryGetSelectedCaptureScreenRect(out RectInt captureRect) ||
				captureRect.width <= 0 ||
				captureRect.height <= 0)
			{
				return;
			}

			float scaleX = target.width / (float)captureRect.width;
			float scaleY = target.height / (float)captureRect.height;
			float x = (_snapshot.ScreenPosition.x - captureRect.x - frame.Hotspot.x) * scaleX;
			float width = frame.Size.x * scaleX;
			float height = frame.Size.y * scaleY;
			float y = target.height - ((_snapshot.ScreenPosition.y - captureRect.y - frame.Hotspot.y) * scaleY) - height;

			if (x >= target.width || y >= target.height || x + width <= 0f || y + height <= 0f)
			{
				return;
			}

			Rect destination = new Rect(x, y, width, height);
			RenderTexture previous = RenderTexture.active;
			bool previousSrgbWrite = GL.sRGBWrite;
			RenderTexture.active = target;
			GL.sRGBWrite = QualitySettings.activeColorSpace == ColorSpace.Linear;
			GL.PushMatrix();
			GL.LoadPixelMatrix(0f, target.width, target.height, 0f);

			Material material = EnsureCursorMaterial();
			if (material != null)
			{
				Graphics.DrawTexture(destination, frame.Texture, new Rect(0f, 0f, 1f, 1f), 0, 0, 0, 0, Color.white, material);
			}
			else
			{
				Graphics.DrawTexture(destination, frame.Texture, new Rect(0f, 0f, 1f, 1f), 0, 0, 0, 0, Color.white);
			}

			GL.PopMatrix();
			GL.sRGBWrite = previousSrgbWrite;
			RenderTexture.active = previous;
		}

		Material EnsureCursorMaterial()
		{
			if (_cursorMaterial != null)
			{
				return _cursorMaterial;
			}

			Shader shader = cursorCompositeShader;
			if (shader == null)
			{
				shader = Shader.Find("Unlit/Transparent");
			}
			if (shader == null)
			{
				shader = Shader.Find("Sprites/Default");
			}
			if (shader == null)
			{
				return null;
			}

			_cursorMaterial = new Material(shader)
			{
				name = "PortailCursor_Composite",
				hideFlags = HideFlags.HideAndDontSave,
			};
			return _cursorMaterial;
		}

		static bool IsValidFrame(CursorFrame frame)
		{
			return frame != null && frame.Texture != null && frame.Size.x > 0 && frame.Size.y > 0;
		}

		bool TryCaptureCursorFrame(IntPtr cursorHandle, out CursorFrame frame)
		{
			frame = null;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!GetIconInfo(cursorHandle, out ICONINFO iconInfo))
			{
				return false;
			}

			try
			{
				IntPtr bitmapHandle = iconInfo.hbmColor != IntPtr.Zero ? iconInfo.hbmColor : iconInfo.hbmMask;
				if (bitmapHandle == IntPtr.Zero ||
					GetObject(bitmapHandle, Marshal.SizeOf<BITMAP>(), out BITMAP bitmap) == 0)
				{
					return false;
				}

				int width = bitmap.bmWidth;
				int height = iconInfo.hbmColor != IntPtr.Zero ? bitmap.bmHeight : bitmap.bmHeight / 2;
				if (width <= 0 || height <= 0 ||
					width > maxCursorTextureSize ||
					height > maxCursorTextureSize)
				{
					return false;
				}

				if (!TryDrawCursorToTexture(cursorHandle, width, height, out Texture2D texture))
				{
					return false;
				}

				frame = new CursorFrame
				{
					Texture = texture,
					Hotspot = new Vector2Int(iconInfo.xHotspot, iconInfo.yHotspot),
					Size = new Vector2Int(width, height),
				};
				return true;
			}
			finally
			{
				if (iconInfo.hbmColor != IntPtr.Zero)
				{
					DeleteObject(iconInfo.hbmColor);
				}
				if (iconInfo.hbmMask != IntPtr.Zero)
				{
					DeleteObject(iconInfo.hbmMask);
				}
			}
#else
			return false;
#endif
		}

		bool TryDrawCursorToTexture(IntPtr cursorHandle, int width, int height, out Texture2D texture)
		{
			texture = null;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			IntPtr dc = CreateCompatibleDC(IntPtr.Zero);
			if (dc == IntPtr.Zero)
			{
				return false;
			}

			IntPtr dib = IntPtr.Zero;
			IntPtr previousObject = IntPtr.Zero;
			try
			{
				BITMAPINFO bitmapInfo = new BITMAPINFO
				{
					bmiHeader = new BITMAPINFOHEADER
					{
						biSize = (uint)Marshal.SizeOf<BITMAPINFOHEADER>(),
						biWidth = width,
						biHeight = -height,
						biPlanes = 1,
						biBitCount = 32,
						biCompression = BiRgb,
					},
				};

				dib = CreateDIBSection(dc, ref bitmapInfo, DibRgbColors, out IntPtr bits, IntPtr.Zero, 0);
				if (dib == IntPtr.Zero || bits == IntPtr.Zero)
				{
					return false;
				}

				previousObject = SelectObject(dc, dib);
				int byteCount = width * height * 4;
				byte[] bgra = new byte[byteCount];
				for (int i = 0; i < bgra.Length; i += 4)
				{
					bgra[i] = 1;
					bgra[i + 1] = 2;
					bgra[i + 2] = 3;
					bgra[i + 3] = 0;
				}

				Marshal.Copy(bgra, 0, bits, byteCount);
				if (!DrawIconEx(dc, 0, 0, cursorHandle, width, height, 0, IntPtr.Zero, DiNormal))
				{
					return false;
				}

				Marshal.Copy(bits, bgra, 0, byteCount);
				byte[] rgba = new byte[byteCount];
				bool anyVisiblePixel = false;
				for (int i = 0; i < bgra.Length; i += 4)
				{
					bool touched = bgra[i] != 1 || bgra[i + 1] != 2 || bgra[i + 2] != 3 || bgra[i + 3] != 0;
					if (!touched)
					{
						continue;
					}

					byte alpha = bgra[i + 3] != 0 ? bgra[i + 3] : (byte)255;
					byte red = bgra[i + 2];
					byte green = bgra[i + 1];
					byte blue = bgra[i];
					rgba[i] = red;
					rgba[i + 1] = green;
					rgba[i + 2] = blue;
					rgba[i + 3] = alpha;
					anyVisiblePixel |= alpha != 0;
				}

				if (!anyVisiblePixel)
				{
					return false;
				}

				texture = new Texture2D(width, height, TextureFormat.RGBA32, false)
				{
					name = "PortailCursor_Frame",
					filterMode = FilterMode.Point,
					wrapMode = TextureWrapMode.Clamp,
					hideFlags = HideFlags.HideAndDontSave,
				};
				texture.LoadRawTextureData(rgba);
				texture.Apply(false, true);
				return true;
			}
			finally
			{
				if (previousObject != IntPtr.Zero)
				{
					SelectObject(dc, previousObject);
				}
				if (dib != IntPtr.Zero)
				{
					DeleteObject(dib);
				}
				DeleteDC(dc);
			}
#else
			return false;
#endif
		}

		void EnsureSystemCursorsHidden()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (_systemCursorsHidden)
			{
				return;
			}

			ReleaseCursorFrames(_hiddenSystemCursorFrames);

			int cursorWidth = Mathf.Max(1, GetSystemMetrics(SmCxCursor));
			int cursorHeight = Mathf.Max(1, GetSystemMetrics(SmCyCursor));
			int maskStride = ((cursorWidth + 15) / 16) * 2;
			byte[] andMask = new byte[maskStride * cursorHeight];
			byte[] xorMask = new byte[andMask.Length];
			for (int i = 0; i < andMask.Length; ++i)
			{
				andMask[i] = 0xFF;
			}

			bool appliedAny = false;
			for (int i = 0; i < SystemCursorIds.Length; ++i)
			{
				uint cursorId = SystemCursorIds[i];
				CursorFrame originalFrame = CaptureSystemCursorFrame(cursorId);
				IntPtr blankCursor = CreateCursor(IntPtr.Zero, 0, 0, cursorWidth, cursorHeight, andMask, xorMask);
				if (blankCursor == IntPtr.Zero)
				{
					DestroyCursorFrame(originalFrame);
					continue;
				}

				if (!SetSystemCursor(blankCursor, cursorId))
				{
					DestroyCursor(blankCursor);
					DestroyCursorFrame(originalFrame);
					continue;
				}

				appliedAny = true;
				IntPtr hiddenCursorHandle = LoadCursor(IntPtr.Zero, new IntPtr(cursorId));
				if (hiddenCursorHandle == IntPtr.Zero || originalFrame == null)
				{
					DestroyCursorFrame(originalFrame);
					continue;
				}

				if (_hiddenSystemCursorFrames.ContainsKey(hiddenCursorHandle))
				{
					DestroyCursorFrame(originalFrame);
					continue;
				}

				_hiddenSystemCursorFrames.Add(hiddenCursorHandle, originalFrame);
			}

			_systemCursorsHidden = appliedAny;
#endif
		}

		CursorFrame CaptureSystemCursorFrame(uint cursorId)
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			IntPtr cursorHandle = LoadCursor(IntPtr.Zero, new IntPtr(cursorId));
			if (cursorHandle == IntPtr.Zero || !TryCaptureCursorFrame(cursorHandle, out CursorFrame frame))
			{
				return null;
			}

			return frame;
#else
			return null;
#endif
		}

		void RestoreSystemCursors()
		{
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
			if (!_systemCursorsHidden)
			{
				return;
			}

			SystemParametersInfo(SpiSetCursors, 0, IntPtr.Zero, 0);
			_systemCursorsHidden = false;
			ReleaseCursorFrames(_hiddenSystemCursorFrames);
#endif
		}

		void ReleaseCursorFrames(Dictionary<IntPtr, CursorFrame> frames)
		{
			if (frames == null || frames.Count == 0)
			{
				return;
			}

			List<CursorFrame> uniqueFrames = new List<CursorFrame>(frames.Values);
			frames.Clear();
			for (int i = 0; i < uniqueFrames.Count; ++i)
			{
				if (uniqueFrames[i] != null && uniqueFrames.IndexOf(uniqueFrames[i]) == i)
				{
					DestroyCursorFrame(uniqueFrames[i]);
				}
			}
		}

		void DestroyCursorFrame(CursorFrame frame)
		{
			if (frame == null || frame.Texture == null)
			{
				return;
			}

			Destroy(frame.Texture);
			frame.Texture = null;
		}
	}
}
