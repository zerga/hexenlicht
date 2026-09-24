# Hexenlicht — development setup

How to go from a fresh clone to running Hexen II from CLion. Building itself
is described in the [README](../../README.md#building).

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

The repository ships two run configurations in `.run/`:

| Configuration | Starts |
|---|---|
| `glhexen2` | Hexen II, windowed 1280×720 |
| `glhexen2 (Portal of Praevus)` | the same with `-portals` |

Pick one in the run configuration box at the top right, pick the
`windows-debug` or `windows-release` profile next to it, and press Run or
Debug. CLion builds the target first.

To change the command line for yourself, copy a configuration
(*Edit Configurations… → Copy*) instead of editing the shared one.

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

## Troubleshooting

- **No run configurations in CLion**: the project was not loaded as a CMake
  project. Right-click `CMakeLists.txt` → *Load CMake Project*, and enable
  the `windows-debug` / `windows-release` profiles
  (*Settings → Build, Execution, Deployment → CMake*).
- **"Hexenlicht's CMake build expects the MSVC toolchain"**: the CMake
  profile uses CLion's bundled MinGW. Use the preset profiles, which select
  the toolchain named *Visual Studio*.
- **`Unknown command "sys_delay"`** and similar lines at startup come from an
  old `config.cfg` written by the original 1997 executables. They are
  harmless.
