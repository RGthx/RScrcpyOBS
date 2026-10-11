# NOTICE

本仓库包含**三个来源**的代码，刻意用"独立进程 + 共享内存"的方式隔开，避免许可冲突：

| 组件 | 位置 | 许可 |
| --- | --- | --- |
| 本项目（OBS 插件、构建与发行脚本） | `obs-plugin/`、`scripts/`、`release/` | **GPL-2.0**（见 [LICENSE](LICENSE)） |
| scrcpy 5.0.1 fork（设备采集端） | `work/scrcpy/` | **Apache-2.0**（见 `work/scrcpy/LICENSE`，上游版权归 Genymobile / scrcpy 贡献者） |
| libobs / OBS Studio（插件运行时链接的对象） | 外部，不随本仓库分发 | **GPL-2.0-or-later** |

## 边界说明

- **OBS 插件**（GPL-2.0）链接 libobs（GPL-2.0-or-later），许可相容。
- **scrcpy fork**（Apache-2.0）是**独立可执行程序**，与插件之间只有：命名共享内存段 + 纯数据结构约定。插件不链接 scrcpy 的任何代码，scrcpy 也不链接 libobs。
- 两侧**共同包含**两个协议头：`obs_shm_proto.h`（音频）与 `obs_video_proto.h`（视频）。它们只含常量、结构体与静态断言，是本项目原创，随本项目以 GPL-2.0 提供；此处明确允许 Apache-2.0 一侧（scrcpy fork）包含使用。协议一旦冻结，改动需两侧同步。

## 第三方依赖（不随仓库分发）

`scripts/fetch-deps.ps1` / `scripts/deps-obs.ps1` 负责从本机既有环境准备 `deps/`：FFmpeg、SDL3、libobs 导入库与头文件。这些依赖各自的许可与版权归其各自作者。
