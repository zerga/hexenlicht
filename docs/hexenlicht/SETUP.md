# Hexenlicht — development setup

How to go from a fresh clone to running Hexen II from CLion. Building itself
is described in the [README](../../README.md#building).

## 0. Vulkan SDK

Install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows),
version 1.3 or newer — ideally the version CI uses (1.4.357.0, see
`.github/workflows/build-windows.yml`). The installer lets you choose the
installation folder; the default components are enough.

The installer sets the `VULKAN_SDK` environment variable. **Restart CLion**
(and any open shells) afterwards so they see it. Older SDKs can stay
installed side by side; only the one `VULKAN_SDK` points to is used.

CMake stops with *"Vulkan SDK 1.3 or newer with ray tracing headers not
found"* if `VULKAN_SDK` is missing or points to an SDK that is too old.

## 1. Game data

Hexenlicht needs the data files of the retail Hexen II, and optionally of the
Portal of Praevus mission pack. They are not included in this repository and
must never be committed.

Create a data folder **next to the repository folder**, named
`Hexenlicht-data`. The shared CLion run configurations expect exactly this
location (`$PROJECT_DIR$/../Hexenlicht-data`):

```
D:\dev\
├── hexenlicht\            ← this repository
└── Hexenlicht-data\
    ├── data1\
    │   ├── pak0.pak       (22,704,056 bytes at v1.11)
    │   └── pak1.pak       (75,601,170 bytes at v1.11)
    └── portals\           ← optional: Portal of Praevus
        └── pak3.pak       (49,089,114 bytes)
```

Copy the `data1` folder (and `portals`, if you have the mission pack) from
your Hexen II installation. Copying the whole folders is fine; the engine
only needs the pak files. Folder name case does not matter on Windows.

Work on a copy, not on your original installation: the engine writes its
configuration, save games and logs into this folder.

## 2. Make sure the paks are version 1.11

Hammer of Thyrion requires the pak files at version 1.11. The original CD
(v1.03) needs patching; many installations are already patched.

1. Build the `h2patch` target (CLion: select `h2patch` and build, or
   `cmake --build --preset windows-debug --target h2patch`).
2. Only if your paks are not yet 1.11: copy
   `gamecode/patch111/patchdat` from this repository into
   `Hexenlicht-data` (so that `Hexenlicht-data\patchdat\data1\*.xd3` exists).
3. Run `build\windows-debug\bin\h2patch.exe` **from inside
   `Hexenlicht-data`**.

For already-patched files `h2patch` prints `Looks like: Retail v1.11 ...
checksumming... OK ... skipped` and changes nothing. A missing `pak2.pak`
is normal (it only exists in an OEM edition).

## 3. Run from CLion

The repository ships these run configurations in `.run/`:

| Configuration | Starts |
|---|---|
| `glhexen2` | Hexen II with the upstream OpenGL renderer, windowed 1280×720 |
| `glhexen2 (Portal of Praevus)` | the same with `-portals` |
| `hexenlicht` | `hexenlicht.exe`, windowed 1280×720, console log in `Hexenlicht-data\debug_h2.log` |
| `hexenlicht (smoke test)` | `hexenlicht.exe -condebug +quit`: initializes the game and quits; the log should end with `Hexen II Initialized` and the config files being executed |

`hexenlicht.exe` runs on Vulkan and draws the 2D screens — console,
menus, status bar, loading plaque, intermissions — but no 3D view yet
(epic E2): the game area stays black. The 2D screen is scaled by a whole
number (`vid_uiscale`, automatic by default: 2x at 1080p, 3x at 1440p, 4x
at 4K; the *Scale* slider in the options menu changes it). Its shaders are loaded from the
`shaders` folder next to the exe (`build\<preset>\bin\shaders`); keep that
folder with the exe when copying it elsewhere. It needs a GPU with Vulkan 1.3 and hardware
ray tracing and says so at startup if there is none. In Debug builds the
Vulkan validation layer is on; its messages go to the console log, and on
exit the log reports `Vulkan validation: N errors, M warnings` — keep that
at zero. Fullscreen is borderless at the
monitor's desktop resolution (`-fullscreen`, or *Fullscreen* in the video
menu); the display mode is never changed. The window can be resized and
maximized; the last normal size is kept for the next start. The process is
per-monitor DPI aware, so window sizes are physical pixels. Its settings are saved to
`hexenlicht.cfg` next to `config.cfg`, so it and `glh2.exe` keep separate
settings; on the first start, when there is no `hexenlicht.cfg` yet, it
reads `config.cfg` (key bindings, mouse, sound) instead.

Pick one in the run configuration box at the top right, pick the
`windows-debug` or `windows-release` profile next to it, and press Run or
Debug. CLion builds the target first.

To change the command line for yourself, copy a configuration
(*Edit Configurations… → Copy*) instead of editing the shared one.

CLion stores the CMake profile of a configuration in these files
(`CONFIG_NAME`, `windows-debug` by default). If CLion rewrites them, for
example after switching profiles, and git shows them as modified, that is
local noise; don't commit it.

## 4. Run without CLion

Start `build\<preset>\bin\glh2.exe` with `Hexenlicht-data` as the working
directory, or pass `-basedir D:\dev\Hexenlicht-data`.

## Useful command-line options

| Option | Effect |
|---|---|
| `-window` / `-fullscreen` | Windowed or fullscreen mode |
| `-width 1280 -height 720` | Video mode size |
| `-portals` | Enable the Portal of Praevus mission pack |
| `-basedir <path>` | Data folder, instead of the working directory |
| `-condebug` | Write the console log to `Hexenlicht-data\debug_h2.log` |
| `+map <name>` | Load a map directly, e.g. `+map demo1` |
| `+quit` | Quit after startup (quick smoke test) |
| `-validation` / `-novalidation` | Force the Vulkan validation layer on (Release) or off (Debug) — `hexenlicht.exe` only |
| `-vkdevice <n>` | Use Vulkan device *n* (the startup log lists them) — `hexenlicht.exe` only |

Console commands and variables of `hexenlicht.exe` so far:

| Command / variable | Effect |
|---|---|
| `vk_info` | Device, driver, ray tracing features, swapchain and validation counts |
| `vk_textures` / `vk_textures list` | Number and memory of loaded textures / every texture with size, mip count and name |
| `map <name>` | Start a map (e.g. `map demo1`); nothing of it is drawn yet, but its textures and models load |
| `vid_vsync 1` / `0` | Wait for vertical blank (default), or present immediately (mailbox) |
| `vid_restart` | Apply `vid_mode` (window size) |
| `vid_uiscale 0` / `n` | Automatic 2D scale (largest whole number keeping the 2D screen at least 640×480), or a fixed factor *n* |
| `screenshot` | Save the next frame as `Hexenlicht-data\data1\shots\hexenNN.tga` |

## Troubleshooting

- **No run configurations in CLion**: the project was not loaded as a CMake
  project. Right-click `CMakeLists.txt` → *Load CMake Project*, and enable
  the `windows-debug` / `windows-release` profiles
  (*Settings → Build, Execution, Deployment → CMake*).
- **"Hexenlicht's CMake build expects the MSVC toolchain"**: the CMake
  profile uses CLion's bundled MinGW. Use the preset profiles, which select
  the toolchain named *Visual Studio*.
- **"Vulkan SDK 1.3 or newer with ray tracing headers not found"**: see
  [section 0](#0-vulkan-sdk). After installing, restart CLion and use
  *Reset Cache and Reload Project* in the CMake panel.
- **`Unknown command "sys_delay"`** and similar lines at startup come from an
  old `config.cfg` written by the original 1997 executables. They are
  harmless.
