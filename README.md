# Sports Replay for OBS Studio

Low-memory instant replay plugin for sports broadcasts.

Sports Replay captures the last seconds of any source into a **compressed**
ring buffer (hardware H.264 all-intra encoding via NVENC/AMF/QSV, with x264
software fallback) instead of storing raw frames in RAM. A 15-second 1080p60
buffer uses on the order of **hundreds of megabytes instead of ~8 GB**,
making multi-camera replay setups practical on ordinary streaming PCs.

## Features

- Instant replay of the last N seconds of any video source
- Slow motion (10%–400% playback speed) and reverse playback
- Multi-camera: add one capture filter per camera, replay any of them
- Hotkeys: capture, play/pause, restart, speed presets, direction toggle
- Compressed in-memory buffer: quality/encoder selectable per filter
- Audio capture and playback (at normal speed)

## How to use

1. Add the **Sports Replay Capture** filter to each source (camera) you want
   to be able to replay. Configure buffer duration and quality there.
2. Add a **Sports Replay** source to the scene where the replay should show.
3. In its properties, select which capture source to replay from.
4. Assign hotkeys (Settings → Hotkeys, under the Sports Replay source):
   *Capture replay* freezes the buffer and starts playback.

## Building

This plugin uses the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system (CMake presets). On Windows:

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Author

Developed by **Systec** — [www.systecinformatica.com.ar](https://www.systecinformatica.com.ar)

## License

GPL-2.0-or-later — Copyright (C) 2026 Systec (https://www.systecinformatica.com.ar)
