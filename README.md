# Portail

Portail is a Unity package for desktop capture, feed playback, input focus/control, and Steam-backed streaming.

## Dependencies

Portail is one package, but it has separate runtime assemblies under `Core`, `Capture`, `Playback`, `Control`, `Stream`, and `Stream/Mirror`.

`Portail.Stream.Mirror.Runtime` has peer dependencies on:

- Mirror, providing the `Mirror` runtime assembly.
- Mirror's Steam transport backend, providing `Mirror.FizzySteam.FizzyFacepunch`.

In `game_night`, these are currently vendored under `Assets/Third Party/Mirror`, so they are marked as peer dependencies in `package.json` instead of strict UPM dependencies to avoid installing duplicate Mirror assemblies.
