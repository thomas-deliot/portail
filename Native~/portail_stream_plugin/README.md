# Portail Stream Unity Native Plugin

This native project builds the Portail desktop streaming plugins used by the Unity package:

- `portail_stream_sender_plugin.dll`
- `portail_stream_receiver_plugin.dll`
- `portail_stream_common.dll`

The older standalone prototype targets have been removed. Runtime entry points are now the Unity C# wrappers under the Portail package `Stream/` folder.

## Build

From this native project folder:

```powershell
.\BuildAndCopyDlls.ps1
```

The build copies Unity plugin DLLs and shared runtime DLLs to the package `Stream/Plugins/` folder.

See `README_UNITY_PLUGIN.md` for Unity-side integration details.
