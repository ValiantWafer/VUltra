# dll

The mod itself: a proxy `openal32.dll` that forwards OpenAL to a copy of the real
dll and, on load, patches `vagante.exe` to add the features. It's all in
`dllmain.cpp`. Settings are read from `vultramod.ini` next to the game exe; the
options are documented inline in that file.

Build with `build2.bat` (needs the VS 2019 x86 BuildTools). The forwarders in
`proxy.def` / `exports.h` are generated from `openal_exports.json`.

See the root [README](../README.md) for install steps and the rest of the project.
