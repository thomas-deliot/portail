# Unity Native Plugin Integration (Current Build)

This branch now builds two native plugins plus a shared runtime DLL:
- `portail_stream_sender_plugin.dll`
- `portail_stream_receiver_plugin.dll`
- `portail_stream_common.dll`

They are copied automatically to:
- `Stream/Plugins/` in the Portail.Network package

## C# wrappers added
- `Stream/PortailStreamSenderPlugin.cs`
- `Stream/PortailStreamReceiverPlugin.cs`

These wrappers expose:
- start/stop
- pause/unpause
- sender bitrate update
- sender window handle update
- sender dynamic receiver SteamID list
- receiver dynamic remote sender SteamID list
- receiver per-sender output texture mapping
- per-frame render-thread GPU copy into a Unity `RenderTexture` via `GL.IssuePluginEvent`
- aggregated and per-sender stats polling

## Data path
- Sender plugin exposes captured pre-encode GPU frames through a shared D3D11 texture.
- Receiver plugin exposes decoded GPU frames through a shared D3D11 texture.
- Unity render thread copies those textures directly into your target `RenderTexture`.
- No GPU->CPU readback is used.

## Runtime model
- Sender DLL: one active runtime per process, can send one captured stream to a dynamic list of receiver SteamIDs.
- Receiver DLL: one active runtime per process, can receive from a dynamic list of sender SteamIDs and route each decoded stream to a different Unity `RenderTexture`.

## EmptyStreamingTest quick start

`Assets/1. Scripts/SteamStreaming/EmptyStreamingTestBootstrap.cs` auto-creates itself at runtime when the active scene is exactly `EmptyStreamingTest`.

- Sender + receiver wrappers are created automatically on one GameObject.
- Streaming starts automatically by default (`autoStartOnPlay = true`).
- Sender preview (pre-encode) and one receiver preview per remote sender are shown in an on-screen overlay.
- A live capture-window picker is shown on the right side of the screen. Clicking a window button immediately switches the sender capture target, and starts sender capture if it was waiting for a window selection.

### Runtime controls
- `P`: pause/resume sender and receiver.
- `=`: increase sender target bitrate.
- `-`: decrease sender target bitrate.
- Inspector context menu on `EmptyStreamingTestBootstrap`: `Start Streaming`, `Stop Streaming`, `Restart Streaming`.

### Native DLLs required in Unity
`Stream/Plugins/` in the Portail.Network package must contain:

- `portail_stream_sender_plugin.dll`
- `portail_stream_receiver_plugin.dll`
- `portail_stream_common.dll`

The same folder must contain the shared native runtime DLLs:

- `steamwebrtc64.dll` (optional, only if you use the ICE path)
- FFmpeg runtime DLLs required by the current build (`avcodec-62.dll`, `avutil-60.dll`, `swresample-6.dll`, `swscale-9.dll`)

`steam_api64.dll` should come from the sender project's existing Facepunch/FizzyFacepunch Steamworks install. The Unity-side native loader resolves that copy first to avoid a duplicate Steam API DLL in the streaming plugin folder.
