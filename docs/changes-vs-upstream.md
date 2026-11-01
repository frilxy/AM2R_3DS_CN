# 本分支相对上游的改动（v0.4.1）

上游：`codeberg.org/NattensMadrigal/AM2R_3DS` 的 `b0c705f`（本仓库的基线提交 `c1d3235` 即其树）。
下面按「玩家能看到什么」和「代码里改了什么」两层列。

## 1. 中文支持

| 项 | 内容 |
|---|---|
| 字库 | `res/n3ds/am2r/overlay/lang/fonts/cjk-font.bin`（124,290 B，1292 字，六种字体共用，1024×512 RGBA5551，1 MB 显存） |
| 装载 | `src/n3ds/n3ds_cjk_font.c`（241 行）：读包 → 建纹理页 → 装 `FontGlyph` → 重建字形 LUT。不再解析 TTF，开机不再读 5 个字体文件 |
| 生成 | `tools/n3ds/fontgen/make_cjk_font.py`（纯标准库解析 EBLC/EBDT，离线跑）、`verify_cjk_font.py` |
| 说明 | `docs/font-plan.md`（包格式、实测表、行距策略）、`docs/localization.md`（现状与缺口） |
| 删除 | 旧分支遗留的 `src/n3ds/n3ds_font_replace.c/.h`（1145 行，未参与编译） |

## 2. 界面与素材

| 项 | 内容 |
|---|---|
| 前端页面 | 显示 / 额外选项 / 金手指 / 传送 四页的标题、行标签、开关值（开/关/不可用）、每行说明 |
| 暂停页 | `[Select] -> 日志` 提示、Display 页说明 |
| `background_replace` | 新实现：标题底图（`bgAM2RTitle`）换成 `lang/titles/` 的中文 logo |
| `sprite_replace` | 新实现：区域标题横幅（按文件名里的帧数切帧 + `removeback` 抠底色）、服装调色板（`mods/palettes/suits/`） |
| `ini.c` | 跳过 UTF-8 BOM（否则 `[Header]` 读不到，语言名解析失败） |
| 资源 | `res/n3ds/am2r/banner.png` 改中文（HOME 菜单）；`overlay/lang/languages.txt` 只留简体中文 |

## 3. 移植层

| 项 | 内容 |
|---|---|
| SELECT | 游戏中打开暂停屏的**道具页**，暂停中返回；START 仍是游戏的菜单页（`n3ds_input.c`，+88 行） |
| 下屏地图 | 游戏中铺满 320×240：去掉上下黑条、地图页标题栏、底部状态栏（时间/剩余 Metroid），约 −105 行 |
| 渲染器 | +228 行：运行时贴图页（`allocImagePage` / `allocImagePageRGBA8` / `allocImageRegionPage` / `flushImagePage`）、文字排版缓存失效、贴图页数组扩容后重指 `image.tex/subtex`（否则全屏文字消失）、内置字体页跳过 `fonts/font_NNNNN.t3x` |
| `stb_impl.c` | 打开 PNG 解码与写 PNG（+5 行） |

## 4. 构建与 SD 卡

| 项 | 内容 |
|---|---|
| CI | SD 工具包带上 `overlay`（字体包、语言文件、横幅） |
| `make_sd.sh` / `sdkit/am2r-sd.sh` | 拷贝 overlay；只保留中文的 `lang/headers`、`lang/titles` |
| 清理 | 删 `.ci/notes-v0.2.*`～`v0.3.*`；`.gitignore` 加 `__pycache__/`、`*.pyc` |

## 5. 与上游一致的部分

- 游戏逻辑、存档、渲染管线主体、暂停页自己的地图版式、`START+SELECT` 调试面板；
- 第一版**不做任何文字位置微调**：行距只在中文降部放不下时才抬高（六种字体的原 stride 见 `docs/font-plan.md`）。
