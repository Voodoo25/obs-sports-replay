# Sports Replay for OBS Studio

**Low-memory instant replay for live sports broadcasts.**

Sports Replay captures the last seconds of any camera into a **compressed**
in-memory buffer (hardware H.264 all-intra via NVENC / AMF / QSV, with an
x264 software fallback) instead of holding raw frames in RAM. A 15-second
1080p60 buffer uses on the order of **tens of megabytes instead of several
gigabytes**, so multi-camera replay setups run comfortably on ordinary
streaming PCs.

<!-- SCREENSHOT: replay bin dock with thumbnails next to the OBS preview -->
<!-- ![Sports Replay dock](docs/dock.png) -->

## Why

The popular raw-buffer replay plugins keep every frame uncompressed in memory
(~8 MB per 1080p frame). With several cameras and a long buffer that easily
reaches many gigabytes and pushes a PC to its limit mid-broadcast. Sports
Replay solves that by encoding the buffer with your GPU, keeping quality high
while cutting memory use by roughly **50–100×**.

## Features

- **Compressed replay buffer** — per-camera capture filter, buffer duration
  and quality configurable.
- **Auto-play on scene switch** — cut to the replay scene and it plays the
  last N seconds automatically.
- **Slow motion & reverse** — playback speed from 10% to 400%, hotkeys for
  speed presets and direction.
- **Configurable end action** — freeze on the last frame, **return to the
  previous scene**, or loop.
- **Sponsor bumpers** — optional intro and outro video clips played around
  the replay (e.g. a "REPLAY" sting with a sponsor).
- **Automatic save to disk** — every replay is saved as an `.mp4` (muxed
  from the already-encoded frames, no re-encode).
- **Replay bin dock** — a dockable panel shows the last replays as
  thumbnails; **double-click sends a saved replay to program** and returns
  to the previous scene when it ends. Perfect for re-showing a play minutes
  later.
- **Multi-camera** — one capture filter per camera; replay any of them.

## Requirements

- OBS Studio 31+ (developed and tested on 32.0.2, Windows).
- A GPU with a hardware H.264 encoder recommended (NVIDIA/AMD/Intel); falls
  back to x264 software encoding otherwise.

## Installation

1. Download the installer from the [latest release](../../releases/latest).
2. Run it (it installs into your OBS plugins folder).
3. Restart OBS.

## How to use

1. **Add the capture filter to each camera.** Right-click a camera source →
   *Filters* → add **Sports Replay Capture**. Set the buffer duration and
   quality there.
2. **Add the playback source.** In your replay scene, add a **Sports Replay**
   source and pick which camera to replay from in its properties. Set the
   playback speed and, under *When the replay ends*, choose *Return to the
   previous scene*. Optionally set intro/outro sponsor clips.
3. **Live replay.** Cut to the replay scene → the last N seconds play
   automatically, then it returns to your main camera.
4. **Replay bin.** Open the **Replays (Sports Replay)** dock. It lists your
   saved replays with thumbnails; double-click any of them to send it to
   program. The save folder is set with the ⚙ button (defaults to
   `Videos/Sports Replay`).

<!-- SCREENSHOT: Sports Replay source properties -->
<!-- ![Source properties](docs/properties.png) -->

Assign hotkeys under *Settings → Hotkeys* (capture, play/pause, speed
presets, reverse, play last saved replay).

## Building from source

Uses the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Author

Developed by **Systec** — [www.systecinformatica.com.ar](https://www.systecinformatica.com.ar)

## License

GPL-2.0-or-later — Copyright (C) 2026 Systec (https://www.systecinformatica.com.ar)
