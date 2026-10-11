# RscrcpyOBS

Phone picture and sound in OBS as one source: an OBS plugin plus a patched scrcpy.

[中文说明](README.zh.md)

---

## 1. Using the patched scrcpy

**Requirements:** Windows 10/11, 64-bit; 64-bit OBS Studio; an Android device with USB
debugging enabled. `adb` and the scrcpy server ship in the package.

The release package unpacks to:

```
RscrcpyOBS-win64-<version>\
  scrcpy\              scrcpy.exe + its runtime DLLs + adb + scrcpy-server
    scrcpy-obs.bat     start this rather than scrcpy.exe
  obs-plugin\          the plugin: build\rscrcpy-audio.dll + data\
  scripts\             install-plugin.ps1 / uninstall-plugin.ps1
```

Start scrcpy through `scrcpy-obs.bat`. Everything after the script name is passed to scrcpy
unchanged.

Mirror the phone into OBS, with its sound:

```
scrcpy\scrcpy-obs.bat --obs-video --obs-video-name dev1 ^
    --obs-audio --obs-audio-name dev1 ^
    --audio-source=playback --audio-codec=raw --no-audio-playback --no-window
```

| Option | Effect |
| --- | --- |
| `--obs-video` | publish the picture to OBS |
| `--obs-video-name=NAME` | the name the picture is published under |
| `--obs-audio` | publish the sound to OBS |
| `--obs-audio-name=NAME` | the name the sound is published under |
| `--audio-source=playback` | capture what the device is playing (Android 13+); `mic`, `mic-unprocessed`, `mic-camcorder` also work |
| `--audio-codec=raw` | no encoder on the phone, no decoder on the PC: the lowest-latency audio |
| `--no-audio-playback` | do not play the sound on the PC as well (listen through OBS instead) |
| `--no-window` | no scrcpy window |

The name: `--obs-video-name` defaults to `--obs-audio-name`, which defaults to `0`. The OBS
source is given the same name.

Everything else is plain scrcpy: `-b 64M`, `-m 1080`, `--max-fps=60`, `-s <serial>`, `-d`,
`--tcpip=...` all behave as they do upstream.

A shortcut whose Target is `<package>\scrcpy\scrcpy-obs.bat <options>` saves retyping; one
shortcut per device or preset.

Sound only:

```
scrcpy\scrcpy-obs.bat --obs-audio --obs-audio-name mic1 --audio-source=mic ^
    --audio-codec=raw --no-audio-playback --no-video
```

Several devices: one scrcpy instance and one OBS source per device, each with its own name.

### In OBS

**Sources** → `+` → **scrcpy Capture (rscrcpy_capture)**:

- **Video shared memory name** — the name you passed;
- **Audio shared memory name** — empty means the same name as the video;
- **Audio offset (ms)** — positive means the sound comes later; use it to align picture and
  sound.

Either order works: a source with no producer yet waits and connects when scrcpy starts.

**scrcpy Audio (rscrcpy-audio)** is an audio-only source (default name `0`) for sound-only
scenes.

---

## 2. Differences from upstream scrcpy

This is scrcpy 5.0.1 ([Genymobile/scrcpy](https://github.com/Genymobile/scrcpy)) with four
options added, plus the behaviour those options need. Every other option, default and
behaviour is upstream.

| New option | Meaning | Default |
| --- | --- | --- |
| `--obs-video` | publish the decoded picture to OBS | off |
| `--obs-video-name=NAME` | the name the picture is published under | the value of `--obs-audio-name` |
| `--obs-audio` | publish the decoded sound to OBS | off |
| `--obs-audio-name=NAME` | the name the sound is published under | `0` |

Further differences:

- `--obs-video` and `--obs-audio` each count as a consumer of their stream. Without them
  scrcpy drops the video (respectively the audio) when nothing plays or records it; with them
  the stream is kept.
- Hardware decoding stays available without a window when `--obs-video` is used. Upstream
  enables hardware decoding only together with video playback.
- `--obs-audio` together with `--no-audio` fails, `--obs-video` together with `--no-video`
  fails, and a name is limited to 64 characters.
- `--obs-video` and `--obs-audio` are Windows-only.

---

## 3. Installing and uninstalling the OBS plugin

**Close OBS first** — the DLL is locked while OBS is running, and both scripts refuse to touch
it otherwise.

Automatic, from the package root (also works from this repository's root):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1

# OBS installed somewhere else (the script's default is D:\obs-studio):
powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1 -ObsRoot "C:\Program Files\obs-studio"
```

Manually — the same two paths the script copies:

```
obs-plugin\build\rscrcpy-audio.dll  ->  <OBS>\obs-plugins\64bit\rscrcpy-audio.dll
obs-plugin\data\*                   ->  <OBS>\data\obs-plugins\rscrcpy-audio\
```

Nothing else is installed, and your scenes and sources are not touched.

Check: restart OBS; the log shows `[rscrcpy-audio] module loaded: version 0.1.1`, and the
source list offers **scrcpy Capture (rscrcpy_capture)** and **scrcpy Audio (rscrcpy-audio)**.

Uninstall:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1
powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1 -PurgeConfig
```

It removes the DLL and `<OBS>\data\obs-plugins\rscrcpy-audio\`. The per-source settings live in
`%APPDATA%\obs-studio\plugin_config\rscrcpy-audio` and are kept unless you pass `-PurgeConfig`.
Both scripts take `-ObsRoot`.

In this repository, `.\obs-plugin\build.ps1 -Install` installs the same two paths into
`D:\obs-studio` (or `-ObsInstall <path>`).

---

## License

- The OBS plugin and the scripts: **GPL-2.0** ([LICENSE](LICENSE)).
- The scrcpy fork (`work/scrcpy/`, shipped as `scrcpy\` in the package): **Apache-2.0**
  ([LICENSE-scrcpy.txt](LICENSE-scrcpy.txt)), originally by Genymobile and the scrcpy
  contributors.
- The plugin and scrcpy are separate processes; neither links the other. See
  [NOTICE.md](NOTICE.md).
