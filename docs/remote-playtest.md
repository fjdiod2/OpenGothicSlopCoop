# Build and run a direct co-op playtest

Each player needs their own Gothic II: Night of the Raven data, the same language,
mods and patch, and the same source revision of this fork. One Mac hosts; one
Windows PC joins. This is a limited playtest, not full-campaign support. World
transitions, ranged/spell controls and unaudited story routes remain unfinished.
The host owns story state and checkpoints; the guest controls the saved companion.

## Build locally

Use the agreed **fork URL and full commit ID**, not an upstream release binary:

```sh
git clone --recurse-submodules YOUR_FORK_URL OpenGothic
cd OpenGothic
git switch -c remote-playtest AGREED_COMMIT_ID
git submodule update --init --recursive
git rev-parse HEAD
```

macOS needs Xcode Command Line Tools and Homebrew dependencies:

```sh
xcode-select --install
brew install cmake ninja glslang openssl@3 python
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target Gothic2Notr create-coop-arena --parallel 6
```

The default Tempest source is the tracked upstream `lib/Tempest`; no background
window extension is required. A fresh build directory avoids carrying a developer
`OPENGOTHIC_TEMPEST_SOURCE_DIR` override. On this project's Mac the separate verified
stock build is `build-stock/opengothic/Gothic2Notr`; use `--engine` to select it.

On Windows, install MSYS2 and the Vulkan SDK as used by the repository's build
workflow. Open the **MSYS2 MINGW64** shell, install the development packages, then
run these commands in the fork checkout:

```sh
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-glslang mingw-w64-x86_64-openssl \
  mingw-w64-x86_64-python
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target Gothic2Notr create-coop-arena --parallel 6
cmake -S tests/protocol -B build-protocol -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-protocol --parallel 4
ctest --test-dir build-protocol --output-on-failure
```

Keep the compiler/OpenSSL runtime DLL directory available in `PATH`, or pass
`--runtime-bin C:/msys64/mingw64/bin` to the launcher. CMake builds Tempest's DLL
alongside the game. An absent DLL is a local build/runtime problem, not a network
failure. A native Windows build plus the asset-free connection suite is still
required before R09; Mac cross-compilation does not replace it.

Single-configuration executables are `build/opengothic/Gothic2Notr[.exe]` and
`create-coop-arena[.exe]`. Visual Studio/vcpkg users additionally need OpenSSL
(`vcpkg install openssl:x64-windows` with the vcpkg CMake toolchain); multi-config
executables may be under `build/opengothic/RelWithDebInfo` or `Release`. Pass the
actual game path using `--engine`. The launcher never downloads an engine.

## Optional identical arena

Generate the arena independently on both machines, into a **new copy** of each
installation. The helper copies only Data, _work and system; original assets,
normal-play saves and settings stay untouched. macOS uses APFS clones; Windows
needs space for a copy. No game data is distributed by this fork.

```sh
python3 scripts/prepare-coop-arena.py --data "/path/to/Gothic II" --output "/path/to/Co-op Arena Data"
```

In Windows MINGW64 use `python` instead of `python3` if needed. Native Windows
Python can also run the scripts with `py -3`. `--generator PATH` selects a custom
build location. Existing output/arena files are refused rather than overwritten.
Generator metadata is fixed, including nested archives. The current arena has 10
triangles and six waypoints, 2,492 bytes, SHA-256:
`8330d1cf28ebf0b73b76d19a8b117fd33c733564af936e07928197b707814b43`.
Both players must select their prepared arena-data folder. Different generated
metadata or a different original installation must not bypass compatibility checks.

The arena starts empty. Developers can prepare a disposable checkpoint containing
the audited trainer/vendor/combat fixtures with the project test tools. Ordinary
Host/Join launchers do not enable developer mode or test control. Use such a
checkpoint for the full arena checklist, or play the supported encounters in a
bounded original-world area. The empty arena alone supports movement/emote checks.

## Host and Join

Double-click `scripts/Host Co-op.command` on Mac. On Windows use
`scripts/Join Co-op.bat` with a native Python installation, or run the Python
script from MINGW64. Each launcher starts exactly one game window. Initial prompts
ask for local data and a checkpoint (Host) or public address/invitation (Join).
Command-line examples, run from the fork checkout:

```sh
python3 scripts/coop-launch.py host --data "/path/to/Co-op Arena Data" --new --world coop_test.zen --port 27500
python3 scripts/coop-launch.py join --data "/path/to/Co-op Arena Data" --address HOST_PUBLIC_IPV4:27500
```

Host defaults to listening on all IPv4 interfaces (`--listen 0.0.0.0`). Tests can
select `--listen 127.0.0.1`. The router must forward TCP 27500 to the Mac's reserved
LAN address. The address printed for listening is local; the friend needs the
router's current **public** address. Forwarding and the host firewall permission
must be arranged by the host. No VPN, relay or automatic router changes are made.

Host generates a random invitation in its profile's `invitation.txt`; open that
file and share its contents privately. Join accepts the value with hidden input,
or `--secret-file PATH`. The secret is never printed, placed in process arguments,
or written to engine logs. It is retained in the user's profile when entered
interactively; an external `--secret-file` remains external. Keep it private. To
rotate the invitation, close both games and remove the host profile's invitation
file; Host creates a new one next time. Update the guest's saved invitation too.
TLS 1.3 authenticates and encrypts the connection before any save transfer.

Default profile locations:

- Mac: `~/Library/Application Support/OpenGothic Co-op/default/host` or `join`.
- Windows: `%LOCALAPPDATA%\OpenGothic Co-op\default\host` or `join`.
- Linux: `$XDG_STATE_HOME/opengothic-coop/default/host` or `join` (default `~/.local/state`).

Use `--profile NAME` for another campaign and `--state-dir PATH` for another base
folder. Only one launcher can own a role/profile at a time. Data and last Join
address persist in `launcher.json`; settings persist in `Gothic.ini`. First-run
defaults are windowed, 30 FPS, 50% render scale and normal audio. Later settings
are preserved. The installation and normal-play runtime are never used for writes.

Save in the **host's game menu**. To restart from a checkpoint:

```sh
python3 scripts/coop-launch.py host --data "/path/to/Co-op Arena Data" --load "/path/to/host/save_slot_0.sav"
```

The launcher imports the checkpoint into a fresh co-op save slot, preserving both
the source file and prior profile saves. Start Join again with the same invitation;
the host restores both players and grants the same companion a fresh session.
After a connection drop or host in-game load, press **R** in the guest window.
If the public address changed, restart Join with the new `--address`. Closing one
launcher never closes the other machine's game. Closing a game or Ctrl-C does not
autosave. Logs remain as `log.txt` and timestamped `launch-*.log` in the profile.

Connection status and useful errors appear in-game and in the launcher terminal.
For build mismatch, compare the full source commit and `Coop: build=` entries.
For data mismatch, compare edition/language/mods, arena hash and `Coop: ... data=`;
use equivalent installations rather than disabling checks. Authentication failure
means the invitation differs. Refused connections/timeouts need the address, live
Host listener, port forwarding and firewall checked. A port check from the same
LAN is not the required two-person internet test.
