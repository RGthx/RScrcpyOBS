# RscrcpyOBS

手机的画面和声音，在 OBS 里是同一个源：一个 OBS 插件 + 一个魔改版 scrcpy。

[English](README.md)

---

## 1. 怎么用魔改版 scrcpy

**前提：** Windows 10/11 64 位；64 位 OBS Studio；一台开了 USB 调试的 Android 设备。
`adb` 与 scrcpy server 都在包里。

发行包解压后是：

```
RscrcpyOBS-win64-<版本>\
  scrcpy\              scrcpy.exe + 运行期 DLL + adb + scrcpy-server
    scrcpy-obs.bat     启动它，而不是 scrcpy.exe
  obs-plugin\          插件：build\rscrcpy-audio.dll + data\
  scripts\             install-plugin.ps1 / uninstall-plugin.ps1
```

通过 `scrcpy-obs.bat` 启动 scrcpy。脚本名后面的参数原样转交给 scrcpy。

把手机镜像进 OBS，并带上声音：

```
scrcpy\scrcpy-obs.bat --obs-video --obs-video-name dev1 ^
    --obs-audio --obs-audio-name dev1 ^
    --audio-source=playback --audio-codec=raw --no-audio-playback --no-window
```

| 参数 | 作用 |
| --- | --- |
| `--obs-video` | 把画面发布给 OBS |
| `--obs-video-name=名字` | 画面发布时用的名字 |
| `--obs-audio` | 把声音发布给 OBS |
| `--obs-audio-name=名字` | 声音发布时用的名字 |
| `--audio-source=playback` | 采集设备正在播放的声音（需 Android 13+）；`mic`、`mic-unprocessed`、`mic-camcorder` 也可以 |
| `--audio-codec=raw` | 手机不编码、电脑不解码：延迟最低的一档音频 |
| `--no-audio-playback` | 电脑上不再播放一遍（改从 OBS 监听） |
| `--no-window` | 不显示 scrcpy 窗口 |

名字：`--obs-video-name` 默认取 `--obs-audio-name`，后者默认是 `0`。OBS 源里填同一个名字。

其余参数都是原版 scrcpy 的：`-b 64M`、`-m 1080`、`--max-fps=60`、`-s <序列号>`、`-d`、
`--tcpip=...` 的行为都与上游一致。

做个快捷方式，"目标"写 `<包目录>\scrcpy\scrcpy-obs.bat <参数>` 就不用每次重敲；一个设备
或一种用途一个。

只要声音：

```
scrcpy\scrcpy-obs.bat --obs-audio --obs-audio-name mic1 --audio-source=mic ^
    --audio-codec=raw --no-audio-playback --no-video
```

多台设备：每台一个 scrcpy 实例 + 一个 OBS 源，各自用自己的名字。

### 在 OBS 里

**来源** → `+` → **scrcpy 采集 (rscrcpy_capture)**：

- **视频共享内存名** —— 填你传的名字；
- **音频共享内存名** —— 留空表示与画面同名；
- **音频偏移（毫秒）** —— 正值表示声音更晚；用它对齐画面和声音。

先后顺序无所谓：还没有采集进程的源会等待，scrcpy 一起来就连上。

**scrcpy 音频 (rscrcpy-audio)** 是纯音频源（默认名 `0`），供只要声音的场景使用。

---

## 2. 跟原版 scrcpy 的区别

这就是 scrcpy 5.0.1（[Genymobile/scrcpy](https://github.com/Genymobile/scrcpy)），加了四个
参数以及这些参数需要的行为。其他所有参数、默认值和行为都保持上游原样。

| 新参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--obs-video` | 把解码后的画面发布给 OBS | 关 |
| `--obs-video-name=名字` | 画面发布时用的名字 | 取 `--obs-audio-name` |
| `--obs-audio` | 把解码后的声音发布给 OBS | 关 |
| `--obs-audio-name=名字` | 声音发布时用的名字 | `0` |

其他差异：

- `--obs-video` 与 `--obs-audio` 各自算这条流的消费者。没有它们时，没人播放/录制的话 scrcpy
  会关掉视频（音频同理）；有它们就保留。
- 带 `--obs-video` 时，没有窗口也能用硬件解码。上游只在有视频播放时才启用硬件解码。
- `--obs-audio` 配 `--no-audio` 会失败，`--obs-video` 配 `--no-video` 会失败，名字上限
  64 个字符。
- `--obs-video` / `--obs-audio` 只在 Windows 上有效。

---

## 3. OBS 插件的安装与卸载

**先关掉 OBS** —— OBS 运行时 DLL 是锁住的，两个脚本在这种情况下都会拒绝动手。

在包目录（在仓库根目录也一样）里自动安装：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1

# OBS 装在别处（脚本默认目标是 D:\obs-studio）：
powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1 -ObsRoot "C:\Program Files\obs-studio"
```

手动安装 —— 也就是脚本复制的那两个路径：

```
obs-plugin\build\rscrcpy-audio.dll  ->  <OBS>\obs-plugins\64bit\rscrcpy-audio.dll
obs-plugin\data\*                   ->  <OBS>\data\obs-plugins\rscrcpy-audio\
```

不装别的东西，也不碰你的场景和源。

确认装上：重启 OBS，日志里会显示 `[rscrcpy-audio] module loaded: version 0.1.1`，来源列表里
能看到 **scrcpy 采集 (rscrcpy_capture)** 与 **scrcpy 音频 (rscrcpy-audio)**。

卸载：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1
powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1 -PurgeConfig
```

它会删掉 DLL 和 `<OBS>\data\obs-plugins\rscrcpy-audio\`。每个源的设置在
`%APPDATA%\obs-studio\plugin_config\rscrcpy-audio`，不加 `-PurgeConfig` 就保留。两个脚本都
接受 `-ObsRoot`。

在本仓库里，`.\obs-plugin\build.ps1 -Install` 会把同样这两个路径装进 `D:\obs-studio`
（或 `-ObsInstall <路径>`）。

---

## 许可

- OBS 插件与脚本：**GPL-2.0**（[LICENSE](LICENSE)）。
- scrcpy fork（`work/scrcpy/`，在包里是 `scrcpy\`）：**Apache-2.0**
  （[LICENSE-scrcpy.txt](LICENSE-scrcpy.txt)），版权归 Genymobile 与 scrcpy 贡献者。
- 插件与 scrcpy 是两个独立进程，不互相链接。见 [NOTICE.md](NOTICE.md)。
