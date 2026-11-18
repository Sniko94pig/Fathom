# Fathom

A mod menu for Subnautica 2 with a clean, minimal look: survival cheats, no-clip and fly, free camera, creature and item ESP, an item spawner, a photo map of the seabed with teleport, base tools, and a Legit Run mode that unlocks things the normal way, step by step.

![Survival page](shots/1-survival.png)

## Install

1. Download `FathomSetup.exe` from the [latest release](https://github.com/Sniko94pig/Fathom/releases/latest).
2. Run it and press **Install**. Fathom goes into `C:\Users\<you>\Fathom` and you get a **Fathom** shortcut on your desktop.
3. Start Subnautica 2, open Fathom from the shortcut and press **Inject**.
4. In the game, **Right Shift** opens the menu and **End** unloads it.

Windows may say the installer is from an unknown publisher, because it isn't signed. Click **More info**, then **Run anyway**.

Running the installer again updates Fathom to the newest version.

## Uninstall

Open **Windows Settings > Apps**, find **Fathom** and choose **Uninstall**, or run **Uninstall Fathom** in the Fathom folder. Your save backups stay in the Fathom folder.

## Good to know

- Cheats only work in single player or when you are the host.
- Before anything permanent, like unlocking everything, the menu backs up your saves to `C:\Users\<you>\Fathom\save-backups`.
- Turn the unlock-all switches off before you save if you want to keep your normal progress.
- This is a fan project. It is not made or supported by Unknown Worlds or Krafton.

## Build from source

Needs Visual Studio 2022 with C++ and CMake 3.24 or newer. The libraries are downloaded during the first build.

```
cmake -S . -B build
cmake --build build --config Release --target fathom fathom_launcher fathom_setup
```

This builds `fathom.dll` (the menu), `Fathom.exe` (the launcher) and `FathomSetup.exe` (the installer).

## Thanks

[Dear ImGui](https://github.com/ocornut/imgui), [MinHook](https://github.com/TsudaKageyu/minhook), [kiero](https://github.com/Rebzzel/kiero), [nlohmann/json](https://github.com/nlohmann/json), [stb](https://github.com/nothings/stb), the [Inter](https://rsms.me/inter/) font and [Lucide](https://lucide.dev) icons.
