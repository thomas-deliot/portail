# Portail

Portail is split into two Unity packages that live in this repository:

- `Portail.Local`: local desktop capture, feed playback, and local input focus/control.
- `Portail.Network`: Steam-backed streaming and future network control.

## Packages

`Portail.Local` contains these runtime assemblies:

- `Portail.Local.Core.Runtime`
- `Portail.Local.Capture.Runtime`
- `Portail.Local.Playback.Runtime`
- `Portail.Local.Control.Runtime`

`Portail.Network` contains these runtime assemblies:

- `Portail.Network.Stream.Runtime`
- `Portail.Network.Control.Runtime`

## Dependencies

`Portail.Local` has no Mirror, FizzySteamworks, or Steam networking dependency.

`Portail.Network` depends on `Portail.Local` and has peer dependencies on:

- Mirror, providing the `Mirror` runtime assembly.
- Mirror's Steam transport backend, providing `Mirror.FizzySteam.FizzyFacepunch`.

In `game_night`, these are currently vendored under `Assets/Third Party/Mirror`, so they are marked as peer dependencies in `Portail.Network/package.json` instead of strict UPM dependencies to avoid installing duplicate Mirror assemblies.
