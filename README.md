# V Ultra

A mod for [Vagante](https://store.steampowered.com/app/323220/Vagante/). Lets you
edit which spells show up in spellbooks, turn cut spells back on, run an endless
looping mode with adjustable difficulty and enemy density, change how fairies and
the mage work, randomize loadouts, let people join a game mid-run, and a few other
things.

It works as a proxy `openal32.dll`. The proxy passes every OpenAL call through to a
copy of the real dll, and on load it patches a handful of functions inside
`vagante.exe`. Addresses are read from the live module, so it survives ASLR, and
every patch checks the bytes it's about to overwrite first.

You set everything in `vultramod.ini`, either by hand or with the manager app.

This repo only contains my own code. No game files, art, or symbols are in here.
You need your own copy of Vagante.

<img width="364" height="860" alt="image" src="https://github.com/user-attachments/assets/b9a581e0-3f43-4e26-8ae2-810f6f162e47" />

## Layout

- `dll/` - the mod. `dllmain.cpp` is the whole thing. `proxy.def`, `exports.h`, and
  `openal_exports.json` set up the OpenAL forwarders.
- `manager/` - a small WinForms app (.NET 9) that installs the mod and edits the ini.
- `re/` - the Python scripts I used to find addresses in the game. They read
  `vagante.pdb`, which isn't included.
- `_local/` - gitignored scratch (game data, RE dumps, packaged builds).

## Build

Needs the MSVC x86 toolchain (VS 2019 BuildTools) and the .NET 9 SDK.

1. In `dll/`, run `build2.bat`. This makes `openal32.dll`.
2. Copy `dll/openal32.dll` to `manager/VUltra.dll` (the manager embeds it).
3. In `manager/`, run `dotnet build -c Release`.

## Install

Easiest is the manager app: it finds your Steam install and has an Install button.

By hand: back up `openal32.dll` as `openal32.orig.dll`, copy it to
`openal32_real.dll`, then drop the built proxy in as `openal32.dll`. Put
`vultramod.ini` next to `vagante.exe`. Redo this after a Steam update or a file
verify, which both wipe `openal32.dll`.

Options are documented inline in `dll/vultramod.ini`.

## WARNING

Because Vagante is a compiled binary file, this uses DLL injection to make these changes. 
Similar methods are used for a lot of cheat engines. 
Do not use this while playing games that sense cheat engines and ban you, 
such as Counter-Strike, League of Legends, Fortnite, etc.

## License

Public domain, [The Unlicense](LICENSE). Do whatever you want with it.

It's a fan mod, not affiliated with or endorsed by the developers, and it gives you
no rights to the game itself.
