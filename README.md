# AM2R 3DS 中文版

在 **New 3DS / New 2DS XL** 上玩 **AM2R**（Another Metroid 2 Remake），中文汉化、下屏地图、宽屏适配。

![着陆点](docs/screenshots/landing-site.png)
![熔岩区](docs/screenshots/lava.png)

> [!IMPORTANT]
> 需要你自己的 AM2R 游戏数据。**游戏本体不包含在本项目中**。
>
> 本分支是**中文整合版**：上屏跑游戏与暂停菜单，下屏显示实时地图；全部界面文字为简体中文。

## 你需要

- 已破解（CFW）的 New 3DS / New 3DS XL / New 2DS XL，并装有 **FBI**
- 你自己的 AM2R 1.1 或 Community Updates 1.5.5 的 `data.win`

## 安装

1. 把 `3ds` 文件夹放到 SD 卡根目录，得到 `sdmc:/3ds/am2r/`（内含 `data.win`、`gfx`、`audio`、`lang` 等）。
2. 用 FBI 安装 `am2r.cia`，或在 Homebrew Launcher 里运行 `am2r.3dsx`。
3. 首次启动会读取 `sdmc:/3ds/am2r/config.ini`，默认语言为简体中文。

## 操作

| 按键 | 功能 |
|---|---|
| A / B / X / Y | 游戏内动作（沿用游戏原设置） |
| L / R / ZL / ZR | 瞄准 / 切换导弹等 |
| **START** | 暂停（地图页），再按一次返回游戏 |
| **SELECT** | 暂停（道具页），再按一次返回游戏 |
| 下屏触摸 | 拖动地图；点击放置/取消标记 |
| START + SELECT | 切换调试信息显示 |

## 已实现的中文化

- `font_replace` / `background_replace` 完整实现，中文以点阵字体渲染，排版与原版一致
- 全部界面文字、道具说明、任务简报、暂停菜单已汉化
- 任务简报等自动折行处的丢字问题已修复
- HOME 菜单横幅与图标使用中文标题

## 画面模式

暂停菜单 → 显示 → 画面模式：

- **点对点**：原生 320×240 居中，两侧留黑边（默认）
- **宽屏**：填满上屏，裁掉左右各 13 像素
- **拉伸**：横向拉伸填满

## 数据来源与致谢

- **AM2R** 由 DoctorM64 制作；**AM2R Community Updates** 由社区维护
- 3DS 移植基于 [nattensmadrigal/AM2R_3DS](https://github.com/nattensmadrigal/AM2R_3DS)（Butterscotch / devkitARM）
- 汉化、下屏地图、画面模式与本文档中的改动由本项目完成

开发细节见 [开发文档.md](开发文档.md)。
