<div align=center>

<img src="extras/banner.png" alt="Banner" width="25%">

</div>
<h1 align=center>Half-Life 2 - Nintendo Switch port</h1>

This is a wrapper/port of the Android version of Half-Life 2 (v1.16.29 - 1.17.0025, nillerusr Source engine port). <br/>
It loads the original game's ARM64 libraries, and runs them as-is in a minimalist fake-Android environment.

### How to install

You're going to need:
* the Source Engine `.apk` [HERE](https://arumoon.github.io/source-engine-download/) and Half-Life 2 Files (from a legally 
 obtained copy of the game)

To install, create `/switch/hl2_nx/` on your SD card and lay it out like this:

```
/switch/hl2_nx/
  hl2_nx.nro
  lib/                  <- all 27 .so files from the APK's lib/arm64-v8a/
  lib/episodic/         <- EP1/EP2 libclient.so and libserver.so from the episodic APK
  assets/extras_dir.vpk <- from the APK's assets/
  files/                <- the fonts from the APK's assets/ (dejavusans.ttf,
                           dejavusans-bold.ttf, DroidSansFallback.ttf,
                           LiberationMono-Regular.ttf, Itim-Regular.otf)
  hl2/                  <- from the game data (the big one: hl2_pak, textures, sounds, maps...)
  episodic/             <- episode 1 (Optional)
  ep2/                  <- episode 2 (Optional)
  platform/             <- from the game data
```

Episode One / Two need episodic `libclient.so` and `libserver.so` in `lib/episodic/` from [HERE](https://nc.workbench.network/s/gdRnpcc2zrXz5Xg/download?path=%2F&files=episodic-1.06_96.apk).

### Notes

This will not work in applet/album mode — use a game override (hold R on a title) or a forwarder.
The Switch needs the full application memory pool

A `config.txt` is created next to the nro on first run:
* `screen_width` / `screen_height` — passed as `-w`/`-h` when both set; `-1` lets the engine pick
* `gamedir` — game to launch: `hl2` (default), `episodic` (Episode One), or `ep2` (Episode Two)
* `args` — extra Source command line
* `lang` — Source language name (default `english`)


### How to build

You need devkitA64 plus the following pacman packages:
* `switch-sdl2`
* `switch-sdl2_image`
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-zlib`
* `switch-ffmpeg`

### Credits

* nillerusr for the Android Source engine port this wraps
* TheOfficialFloW & fgsfds for the so-loader lineage
* Masagrator for the hl2_picker

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with Valve Corporation. "Half-Life" and "Source" are trademarks
of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original
game.

Unless specified otherwise, the source code provided in this repository is licensed under the MIT
License. Please see the accompanying LICENSE file.

### SaltyNX telemetry

FPS and render resolution are published to SaltyNX shared memory for compatible overlays. The port continues normally when SaltyNX is not running. This reports presentation timing and resolution; it does not change the game's frame cap.
The old wrapper FPS overlay and its `show_fps` setting have been removed. Existing config entries are ignored; use a SaltyNX-compatible overlay to view FPS.

The in-game video options support brightness adjustment. Game settings and bindings are separate from the launcher's `config.txt`: each game stores its own settings in `hl2/cfg/config.cfg`, `episodic/cfg/config.cfg`, or `ep2/cfg/config.cfg`. Use the in-game Quit option to let the engine finish saving. An `autoexec.cfg` or `userconfig.cfg` can override saved settings.

### Controller defaults

HL2 and both episodes use these controls with paired Joy-Con or a Pro Controller:

| Switch control | Action |
| --- | --- |
| B / A | Jump / reload |
| Y / X | Use / flashlight |
| L / R | Sprint / gravity-gun quick swap |
| ZL / ZR | Secondary / primary fire |
| Left-stick / right-stick click | Toggle crouch / suit zoom |
| Minus / Plus | Squad orders / pause |
| D-pad | Weapon selection |

Empty and unchanged stock controller profiles receive these defaults. Customized saved profiles keep their bindings, sensitivity and axis settings. Overrides in the selected game's `cfg/autoexec.cfg` take priority, including `unbind` and scripts executed from autoexec. Source uses SDL button names: `A_BUTTON`, `B_BUTTON`, `X_BUTTON`, `Y_BUTTON` correspond to Switch B, A, Y, X respectively.

Use `exec switch_controller.cfg` in the game console or autoexec to restore the default button bindings. This command is built into the port and requires no additional file. The engine saves the resulting bindings normally.

### Loading

Read-only game archives use a RAM cache of up to 64 MiB to avoid repeated SD card reads. Memory is allocated as needed; normal reads remain available if allocation fails. Save files and configuration files keep their usual I/O behavior.

The cache regression test runs on Linux:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Isource \
  tests/archive_cache_test.c source/archive_cache.c source/archive_io.c -pthread \
  -Wl,--wrap=malloc,--wrap=fread,--wrap=fseeko -o /tmp/hl2-archive-cache-test
/tmp/hl2-archive-cache-test full
/tmp/hl2-archive-cache-test oom
/tmp/hl2-archive-cache-test limited
```
