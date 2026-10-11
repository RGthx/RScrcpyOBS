RscrcpyOBS - phone picture and sound in OBS as one source
================================================================================

An OBS plugin plus a patched scrcpy.

CONTENTS
  scrcpy\             the patched scrcpy: scrcpy.exe + runtime DLLs + adb + server
    scrcpy-obs.bat    start this rather than scrcpy.exe
  obs-plugin\         the plugin (build\rscrcpy-audio.dll + data\)
  scripts\            install-plugin.ps1 / uninstall-plugin.ps1
  README.txt          this file
  VERSION.txt, LICENSE, LICENSE-scrcpy.txt, NOTICE.md

================================================================================
1. USING THE PATCHED SCRCPY
================================================================================

Requirements: Windows 10/11 64-bit, 64-bit OBS Studio, and an Android device with
USB debugging enabled. adb and the scrcpy server ship in this folder.

Start scrcpy through scrcpy-obs.bat. Everything after the script name is passed
to scrcpy unchanged.

Mirror the phone into OBS, with its sound:

  scrcpy\scrcpy-obs.bat --obs-video --obs-video-name dev1 ^
      --obs-audio --obs-audio-name dev1 ^
      --audio-source=playback --audio-codec=raw --no-audio-playback --no-window

  --obs-video / --obs-video-name=NAME   publish the picture to OBS, under NAME
  --obs-audio / --obs-audio-name=NAME   publish the sound to OBS, under NAME
  --audio-source=playback               capture what the device is playing
                                        (Android 13+; mic / mic-unprocessed /
                                        mic-camcorder also work)
  --audio-codec=raw                     no encoder on the phone, no decoder on the
                                        PC: the lowest-latency audio
  --no-audio-playback                   do not play the sound on the PC as well
                                        (listen through OBS instead)
  --no-window                           no scrcpy window

The name: --obs-video-name defaults to --obs-audio-name, which defaults to 0.
The OBS source is given the same name.

Everything else is plain scrcpy: -b 64M, -m 1080, --max-fps=60, -s <serial>, -d,
--tcpip=... all behave as they do upstream.

A shortcut whose Target is
  <this folder>\scrcpy\scrcpy-obs.bat <options>
saves retyping; one shortcut per device or preset.

Sound only:

  scrcpy\scrcpy-obs.bat --obs-audio --obs-audio-name mic1 --audio-source=mic ^
      --audio-codec=raw --no-audio-playback --no-video

Several devices: one scrcpy instance and one OBS source per device, each with its
own name.

IN OBS
  Sources -> + -> "scrcpy Capture (rscrcpy_capture)":
    * Video shared memory name  = the name you passed
    * Audio shared memory name  = empty means the same name as the video
    * Audio offset (ms)         = positive means the sound comes later; use it to
                                  align picture and sound
  Either order works: a source with no producer yet waits and connects when
  scrcpy starts.
  "scrcpy Audio (rscrcpy-audio)" is an audio-only source (default name 0) for
  sound-only scenes.

================================================================================
2. DIFFERENCES FROM UPSTREAM SCRCPY
================================================================================

This is scrcpy 5.0.1 (https://github.com/Genymobile/scrcpy) with four options
added, plus the behaviour those options need. Every other option, default and
behaviour is upstream.

  --obs-video              publish the decoded picture to OBS   (default: off)
  --obs-video-name=NAME    the name the picture is published under
                           (default: the value of --obs-audio-name)
  --obs-audio              publish the decoded sound to OBS     (default: off)
  --obs-audio-name=NAME    the name the sound is published under (default: 0)

Further differences:
  * --obs-video and --obs-audio each count as a consumer of their stream. Without
    them scrcpy drops the video (respectively the audio) when nothing plays or
    records it; with them the stream is kept.
  * Hardware decoding stays available without a window when --obs-video is used.
    Upstream enables hardware decoding only together with video playback.
  * --obs-audio together with --no-audio fails, --obs-video together with
    --no-video fails, and a name is limited to 64 characters.
  * --obs-video and --obs-audio are Windows-only.

================================================================================
3. INSTALLING / UNINSTALLING THE OBS PLUGIN
================================================================================

Close OBS first -- the DLL is locked while OBS is running, and both scripts refuse
to touch it otherwise.

  powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1
  powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1 -ObsRoot "C:\Program Files\obs-studio"

The script's default target is D:\obs-studio; pass -ObsRoot for your own.

Or copy the two paths by hand:

  obs-plugin\build\rscrcpy-audio.dll  ->  <OBS>\obs-plugins\64bit\rscrcpy-audio.dll
  obs-plugin\data\*                   ->  <OBS>\data\obs-plugins\rscrcpy-audio\

Nothing else is installed, and your scenes and sources are not touched.

Check: restart OBS; the log shows
  [rscrcpy-audio] module loaded: version 0.1.1
and the source list offers "scrcpy Capture (rscrcpy_capture)" and
"scrcpy Audio (rscrcpy-audio)".

  powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1
  powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1 -PurgeConfig

Uninstall removes the DLL and <OBS>\data\obs-plugins\rscrcpy-audio\. The per-source
settings live in %APPDATA%\obs-studio\plugin_config\rscrcpy-audio and are kept
unless you pass -PurgeConfig. Both scripts take -ObsRoot.

LICENSE
  The plugin and the scripts: GPL-2.0 (LICENSE).
  The scrcpy fork in scrcpy\:  Apache-2.0 (LICENSE-scrcpy.txt), originally by
  Genymobile and the scrcpy contributors.
  The plugin and scrcpy are separate processes; neither links the other.
  See NOTICE.md.

--------------------------------------------------------------------------------
中文
--------------------------------------------------------------------------------

RscrcpyOBS —— 手机的画面和声音，在 OBS 里是同一个源
================================================================================

一个 OBS 插件 + 一个魔改版 scrcpy。

内容
  scrcpy\             魔改版 scrcpy：scrcpy.exe + 运行期 DLL + adb + server
    scrcpy-obs.bat    启动它，而不是 scrcpy.exe
  obs-plugin\         插件（build\rscrcpy-audio.dll + data\）
  scripts\            install-plugin.ps1 / uninstall-plugin.ps1
  README.txt          本文件
  VERSION.txt、LICENSE、LICENSE-scrcpy.txt、NOTICE.md

================================================================================
1. 怎么用魔改版 scrcpy
================================================================================

前提：Windows 10/11 64 位、64 位 OBS Studio、一台开了 USB 调试的 Android 设备。
adb 与 scrcpy server 都在这个文件夹里。

通过 scrcpy-obs.bat 启动 scrcpy。脚本名后面的参数原样转交给 scrcpy。

把手机镜像进 OBS，并带上声音：

  scrcpy\scrcpy-obs.bat --obs-video --obs-video-name dev1 ^
      --obs-audio --obs-audio-name dev1 ^
      --audio-source=playback --audio-codec=raw --no-audio-playback --no-window

  --obs-video / --obs-video-name=名字   把画面发布给 OBS，名字为"名字"
  --obs-audio / --obs-audio-name=名字   把声音发布给 OBS，名字为"名字"
  --audio-source=playback               采集设备正在播放的声音（需 Android 13+；
                                        mic / mic-unprocessed / mic-camcorder 也可以）
  --audio-codec=raw                     手机不编码、电脑不解码：延迟最低的一档音频
  --no-audio-playback                   电脑上不再播放一遍（改从 OBS 监听）
  --no-window                           不显示 scrcpy 窗口

名字：--obs-video-name 默认取 --obs-audio-name，后者默认是 0。
OBS 源里填同一个名字。

其余参数都是原版 scrcpy 的：-b 64M、-m 1080、--max-fps=60、-s <序列号>、-d、
--tcpip=... 的行为都与上游一致。

做个快捷方式，"目标"写：<本文件夹>\scrcpy\scrcpy-obs.bat <参数>，就不用每次重敲；
一个设备或一种用途一个。

只要声音：

  scrcpy\scrcpy-obs.bat --obs-audio --obs-audio-name mic1 --audio-source=mic ^
      --audio-codec=raw --no-audio-playback --no-video

多台设备：每台一个 scrcpy 实例 + 一个 OBS 源，各自用自己的名字。

在 OBS 里
  来源 -> + -> "scrcpy 采集 (rscrcpy_capture)"：
    · 视频共享内存名 = 你传的名字
    · 音频共享内存名 = 留空表示与画面同名
    · 音频偏移（毫秒）= 正值表示声音更晚；用它对齐画面和声音
  先后顺序无所谓：还没有采集进程的源会等待，scrcpy 一起来就连上。
  "scrcpy 音频 (rscrcpy-audio)" 是纯音频源（默认名 0），供只要声音的场景使用。

================================================================================
2. 跟原版 scrcpy 的区别
================================================================================

这就是 scrcpy 5.0.1（https://github.com/Genymobile/scrcpy），加了四个参数以及这些
参数需要的行为。其他所有参数、默认值和行为都保持上游原样。

  --obs-video            把解码后的画面发布给 OBS（默认关）
  --obs-video-name=名字  画面发布时用的名字（默认取 --obs-audio-name）
  --obs-audio            把解码后的声音发布给 OBS（默认关）
  --obs-audio-name=名字  声音发布时用的名字（默认 0）

其他差异：
  · --obs-video 与 --obs-audio 各自算这条流的消费者。没有它们时，没人播放/录制的话
    scrcpy 会关掉视频（音频同理）；有它们就保留。
  · 带 --obs-video 时，没有窗口也能用硬件解码。上游只在有视频播放时才启用硬件解码。
  · --obs-audio 配 --no-audio 会失败，--obs-video 配 --no-video 会失败，名字上限
    64 个字符。
  · --obs-video / --obs-audio 只在 Windows 上有效。

================================================================================
3. OBS 插件的安装与卸载
================================================================================

先关掉 OBS —— OBS 运行时 DLL 是锁住的，两个脚本在这种情况下都会拒绝动手。

  powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1
  powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1 -ObsRoot "C:\Program Files\obs-studio"

脚本默认目标是 D:\obs-studio，装在别处就传 -ObsRoot。

也可以手动复制那两个路径：

  obs-plugin\build\rscrcpy-audio.dll  ->  <OBS>\obs-plugins\64bit\rscrcpy-audio.dll
  obs-plugin\data\*                   ->  <OBS>\data\obs-plugins\rscrcpy-audio\

不装别的东西，也不碰你的场景和源。

确认装上：重启 OBS，日志里会显示
  [rscrcpy-audio] module loaded: version 0.1.1
来源列表里能看到 "scrcpy 采集 (rscrcpy_capture)" 与 "scrcpy 音频 (rscrcpy-audio)"。

  powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1
  powershell -ExecutionPolicy Bypass -File scripts\uninstall-plugin.ps1 -PurgeConfig

卸载会删掉 DLL 和 <OBS>\data\obs-plugins\rscrcpy-audio\。每个源的设置在
%APPDATA%\obs-studio\plugin_config\rscrcpy-audio，不加 -PurgeConfig 就保留。
两个脚本都接受 -ObsRoot。

许可
  插件与脚本：GPL-2.0（LICENSE）。
  scrcpy\ 里的 fork：Apache-2.0（LICENSE-scrcpy.txt），版权归 Genymobile 与 scrcpy 贡献者。
  插件与 scrcpy 是两个独立进程，不互相链接，见 NOTICE.md。
