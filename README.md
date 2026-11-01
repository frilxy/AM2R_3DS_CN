# AM2R for New 3DS（简体中文汉化版）

在 **New 3DS / New 3DS XL / New 2DS XL** 上玩 **AM2R**（Another Metroid 2 Remake）。
本仓库是在 [NattensMadrigal/AM2R_3DS](https://codeberg.org/NattensMadrigal/AM2R_3DS) 移植版基础上做的
**简体中文汉化分支**：界面、字库、标题 logo、区域标题与 HOME 菜单 banner 都是中文，玩法与画面布局保持上游原样。

![萨姆斯在着陆点](docs/screenshots/landing-site.png)
![熔岩区域](docs/screenshots/lava.png)
![下屏地图](docs/screenshots/pause-map.png)

> [!IMPORTANT]
> 你需要自己拥有 AM2R。游戏本体不随本仓库分发。

## 与上游的区别

完整清单见 [docs/changes-vs-upstream.md](docs/changes-vs-upstream.md)，一句话版本：

- **中文化**：离线烘焙的 `cjk-font.bin`（六个字体、1291 字，1 MB 显存）、四个端口页面、`[Select] -> 日志` 等提示、
  标题 logo、区域标题横幅、HOME 菜单 banner；
- **道具名下移 2px**（旧汉化版的排版微调），其余文字位置一律不动；
- **下屏地图满屏**：去掉上下黑条、地图页标题栏与底部状态栏；
- **修掉三个只在实机出现的问题**：运行时贴图页 realloc 后指针失效（全屏文字消失）、
  暂停屏一帧画满 GPU 命令缓冲（按 START 死机）、字形贴图零间距导致 1px 竖笔画丢失；
- 输入层与上游完全一致（不额外占用 SELECT）。

## 你需要

- 已破解（CFW）的 New 3DS / New 3DS XL / New 2DS XL，装有 **FBI**（参考 [3ds.hacks.guide](https://3ds.hacks.guide)）；
- 你自己的 AM2R 1.1 原版压缩包（`AM2R_11.zip`）；
- 一台带桌面浏览器的电脑（Chrome / Edge / Firefox）。

## 安装

**1. 准备 SD 卡数据**

两种方式任选：

- 用**[在线 SD 卡生成器](https://nattensmadrigal.codeberg.page/AM2R_3DS/)**：拖入 `AM2R_11.zip`，它会更新到
  Community Updates 1.5.5 并转成 3DS 版（全程在浏览器里完成，不上传），把生成的压缩包解到 SD 卡根目录；
- 或者用 [Releases](../../releases) 里的 `am2r-sd.sh` / `am2r-sd.bat` 处理你已经更新过的游戏目录。

**2. 放入汉化素材**

从本仓库的 Release 下载 `am2r-sd-tools.tar.gz`，解压后把 `overlay/` 里的内容覆盖到 SD 卡的
`3ds/am2r/`（中文字库 `lang/fonts/cjk-font.bin`、中文语言文件、中文横幅）。若使用生成脚本，脚本会自动处理。

**3. 安装 CIA**

用 FBI 安装 Release 里的 `am2r.cia`：Remote Install → Scan QR Code 扫 `am2r-cia-qr.png`，或直接把 CIA 拷进卡里安装。
也可以用 `am2r.3dsx` 走 Homebrew Launcher。

## SD 卡目录

```
3ds/am2r/
├── data.win              游戏数据（由生成器/脚本转换得到）
├── gfx/                  atlas.bin、direct_assets.bin、room_manifest.bin
├── audio/                *.bcwav
├── lang/
│   ├── languages.txt     只保留简体中文
│   ├── schinese.ini      中文语言文件
│   ├── fonts/            五套 TTF + cjk-font.bin（汉化字库）
│   ├── headers/          区域标题横幅（中文，7 张）
│   └── titles/           标题 logo（中文，1 张）
├── mods/                 调色板等
├── config.ini
└── log.txt               运行日志（反馈问题时请附上）
```

语言在 `config.ini` 的 `[Screen] Language` 里选（本汉化包只带简体中文）。

## 操作

| 按键 | 作用 |
|---|---|
| B | 跳 |
| Y | 射击 |
| A | 变形球 |
| L / ZL | 瞄准 |
| R | 导弹 |
| ZR | 锁定瞄准 |
| Select | 切换武器（交给游戏） |
| Start | 暂停（下屏显示地图） |

触摸下屏可打开地图与暂停菜单。

## 已知限制

- 区域标题：游戏本身没有 `sIntro_A0` / `sIntro_A6` 素材，这两个区域不会播标题；
- 宽屏（426 布局压到 400 上屏）下 12px 中文仍会有轻微横向压缩，属于游戏自身 GUI 布局，本汉化未改动；
- `lang/headers`、`lang/titles` 只保留中文版本，切到其它语言会出现缺图。

## 反馈

请附上 `sdmc:/3ds/am2r/log.txt`；若崩溃，`/luma/dumps/` 下会有 `crash_dump_*.dmp`，一并提供可以定位到具体函数。

## 文档

- [与上游的区别](docs/changes-vs-upstream.md)
- [中文字库设计](docs/font-plan.md)（包格式、实测表、行距与位置策略）
- [汉化现状与缺口](docs/localization.md)
- [发布约定](docs/release-convention.md)

---

AM2R 由 DoctorM64 与团队制作，Community Updates 由 AM2R Community Developers 维护。
本移植基于 NattensMadrigal 的 AM2R_3DS。与任天堂无关。
