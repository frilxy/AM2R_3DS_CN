# AM2R 3DS 中文版

在 **New 3DS / New 2DS XL** 上玩 **AM2R**（Another Metroid 2 Remake）：完整汉化、下屏实时地图、宽屏适配。

[English](README.en.md) · [开发文档](开发文档.md) · [Releases](https://github.com/frilxy/AM2R_3DS_CN/releases)

![着陆点](docs/screenshots/landing-site.png)
![熔岩区](docs/screenshots/lava.png)

> [!IMPORTANT]
> **需要你自己的 AM2R 游戏数据，游戏本体不包含在本项目里。**
> 准备好后按下面的步骤用 `tools/n3ds/make_sd.sh` 生成 SD 卡目录。

## 需要

- 已破解（CFW）的 New 3DS / New 3DS XL / New 2DS XL，装有 **FBI**
- 你自己的 AM2R 1.1 或 Community Updates 1.5.5 的 `data.win`

## 安装

1. 用 `tools/n3ds/make_sd.sh` 从你的 AM2R 目录生成 `3ds` 文件夹，放到 SD 卡根目录，得到 `sdmc:/3ds/am2r/`（约 565 MB）。
2. 用 FBI 安装 `am2r.cia`；或把 `am2r.3dsx` 放到 SD 卡，从 Homebrew Launcher 运行。
3. 首次启动读 `sdmc:/3ds/am2r/config.ini`，`[Screen] Language` 为 `1` 即简体中文（默认）。

从旧版本升级只需覆盖安装 CIA，存档与 SD 数据都不用动。

## 操作

| 按键 | 功能 |
|---|---|
| A / B / X / Y / L / R / ZL / ZR | 游戏内动作（沿用游戏原设置） |
| **START** | 短按：交给游戏（扫描后查看区域说明）；**长按**：暂停 → 地图页 |
| **SELECT** | 暂停 → 道具页，再按一次返回 |
| 下屏触摸 | 拖动地图；点击放置/取消标记 |
| START + SELECT | 调试信息 |
| 暂停菜单 → 显示 | 画面模式（点对点 / 宽屏 / 拉伸）、状态栏、跳帧、快速脚本、帧率显示、金手指 |

## 这个版本做了什么

- **完整汉化**：`font_replace` / `background_replace` 已实现，中文以点阵字体渲染；界面、道具说明、任务简报、暂停菜单全中文，折行丢字已修。
- **下屏实时地图**：状态计数器、缩放档位、拖动平移、点击标记；非游玩界面显示下屏底图。
- **画面模式**：点对点（原生 320×240，默认）、宽屏（填满上屏）、拉伸。
- **性能**：存档选择页掉帧、玩久后崩溃（图集页内存不足）已修；字体会缓存在 SD 卡上，第二次启动快约 1 秒。

## 可以自己改的两处

| 文件 | 作用 |
|---|---|
| `sdmc:/3ds/am2r/gfx/bottom.png` | 下屏底图（320×240 PNG）。放自己的图即可替换，不用重装 CIA |
| `sdmc:/3ds/am2r/fontcache.bin` | 字体缓存，自动生成。删掉会在下次启动重建（怀疑显示异常时可先删它） |

## 构建

本项目**只在 GitHub Actions 编译**（`.github/workflows/build-n3ds.yml`），推送到任何分支都会产出 `am2r.cia` / `am2r.3dsx` 的 pre-release；打 tag 则发布正式版。本地不需要 devkitARM。

## 致谢

- **AM2R** 由 DoctorM64 制作，**AM2R Community Updates** 由社区维护
- 3DS 移植基于 [nattensmadrigal/AM2R_3DS](https://github.com/nattensmadrigal/AM2R_3DS)（Butterscotch / devkitARM / libctru）
- 汉化、下屏地图、画面模式与性能优化由本项目完成

实现细节、已知问题与各版本改动见 [开发文档.md](开发文档.md)。
