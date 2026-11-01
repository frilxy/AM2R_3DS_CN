# 本分支相对上游的改动（v0.4.5）

上游：`codeberg.org/NattensMadrigal/AM2R_3DS`，基线提交 `b0c705f`（本仓库的 `c1d3235` 即其树）。
本文按「玩家能看到什么」「代码里改了什么」「修掉了什么」三面列，最后一节是**明确没改**的部分。

## 一句话

在保持上游玩法、画面布局与操作的前提下，把界面与字库换成简体中文；另外修了三个只在实机出现的问题
（贴图页指针失效、GPU 命令缓冲溢出、字形贴图零间距），并把下屏地图改成满屏。

## 1. 玩家可见的差异

| 项 | 内容 |
|---|---|
| 中文字库 | 开局装载离线烘焙的 `cjk-font.bin`，六个字体（界面/正文/对话框/选项/道具名/小字）全部可显示中文 |
| 前端页面 | 显示 / 额外选项 / 金手指 / 传送 四页：标题、行标签、开关值（开 / 关 / 不可用）、每行说明都是中文 |
| 暂停页提示 | `[Select] -> 日志` 等提示行 |
| 标题 logo | `background_replace` 把 `bgAM2RTitle` 换成 `lang/titles/` 里的中文 logo |
| 区域标题 | `sprite_replace` 把 `sIntro_A1..A8` 换成中文横幅，按帧切分后仍保留原动画 |
| 道具名 | 道具页的道具名整体**下移 2px**（旧汉化版的 `fontMenuTiny` 垂直微调，见 §4） |
| 下屏地图 | 游戏中铺满整个 320×240：去掉上下黑条、地图页标题栏与底部状态栏 |
| HOME 菜单 | `banner.png` 为中文版 |
| SD 卡 | 只保留中文的 `lang/headers`、`lang/titles`；非中文素材在生成脚本里剔除 |

除此之外**没有**移动任何文字位置：行距只在中文降部放不下时才抬高（见 `docs/font-plan.md`）。

## 2. 新增代码

| 文件 | 行数 | 作用 |
|---|---|---|
| `src/n3ds/n3ds_cjk_font.c/.h` | +267 | 读 `cjk-font.bin` → 建一张 1024×512 RGBA5551 贴图页 → 装 `FontGlyph` 表 → 重建字形 LUT。开机不再解析 TTF |
| `src/n3ds/n3ds_am2r.c` | +424 | 四个端口页面与其中文化、`background_replace`/`sprite_replace` 实现、传送/作弊逻辑、`draw_text` 钩子 |
| `src/n3ds/n3ds_renderer.c/.h` | +265 | 运行时贴图页（`allocImagePage` / `allocImagePageRGBA8` / `allocImageRegionPage` / `flushImagePage`）、排版缓存失效、贴图页数组扩容后重指指针、内置字体页跳过 `fonts/font_NNNNN.t3x`、命令缓冲提交 |
| `src/n3ds/n3ds_livemap.c` | −105 | 下屏地图满屏（去掉条带与文字） |
| `src/ini.c` | +5 | 跳过 UTF-8 BOM（否则 `[Header]` 读不到，语言名乱码） |
| `src/n3ds/stb_impl.c` | +5 | 打开 PNG 解码/编码（`background_replace`/`sprite_replace` 读 PNG） |
| `src/n3ds/main.c` | +9 | GPU 命令缓冲 512 KB → 4 MB（失败依次退 1 MB / 512 KB） |
| `src/n3ds/n3ds_font_replace.c/.h` | −1181 | 删掉旧分支遗留的运行时点阵解析（未参与编译） |

合计 13 个文件，**+942 / −1325 行**（净减 383 行）。

## 3. 资源与工具链

| 项 | 内容 |
|---|---|
| 字库产物 | `res/n3ds/am2r/overlay/lang/fonts/cjk-font.bin`：129,922 B，1291 字，六种字体共用，1024×512 RGBA5551（1 MB 显存） |
| 生成器 | `tools/n3ds/fontgen/make_cjk_font.py`（纯标准库解析 TTF 的 EBLC/EBDT，离线运行） |
| 校验 | `tools/n3ds/fontgen/verify_cjk_font.py`（按 C 加载器的偏移重读，渲染排版检查） |
| 设计文档 | `docs/font-plan.md`（包格式、实测表、行距策略、限制） |
| SD 生成 | `tools/n3ds/make_sd.sh`、`tools/n3ds/sdkit/am2r-sd.sh`：拷贝 overlay、剔除非中文标题/横幅 |
| CI | `.github/workflows/build-n3ds.yml`：SD 工具包里带上 `overlay` |
| 清理 | 删掉旧的 `.ci/notes-*`；`.gitignore` 加 `__pycache__/`、`*.pyc` |

## 4. 修掉的问题（含只在实机出现的）

| 问题 | 现象 | 原因与修法 |
|---|---|---|
| 贴图页指针失效 | 加完 `sprite_replace` 后**全屏文字消失**，logo 正常 | 运行时贴图页的 `C2D_Image` 指向自己条目里的字段，而条目池是 `realloc` 出来的数组；一扩容旧地址全失效。在分配后统一重指 `image.tex/subtex` |
| 区域横幅只闪一下 | 播区域标题时整条横幅闪一下 | 语言包的横幅是「一条 = N 帧并排」，文件名里的 `f16` 是**帧数**（实测墨迹周期 52px = 832/16），按帧切开重建精灵的帧 |
| 横幅带黑底 | 横幅周围有黑框 | `sprite_replace` 的 `removeback` 之前被忽略；按 GameMaker 约定取左下角像素颜色抠像 |
| 字形缺竖笔画 | 设置页等 12px 中文缺一像素竖笔画 | 打包时字形矩形彼此紧贴（1277 处 0 间距），相邻字形的空白首列会被采样进来；生成器改成每个字形右边留 **1px**（贴图 404 → 448 行，仍是 1024×512） |
| 实机按 START 死机 | 打开暂停屏立刻崩溃，模拟器不崩 | citro3d 的 GPU 命令缓冲（512 KB）被暂停地图一帧画满，`GPUCMD_Add` 内部 `svcBreak`。崩溃转储：`pc=svcBreak`、`lr=GPUCMD_Add+0xb8`、`r4=&gpuCmdBufOffset`、`r10=&gpuCmdBufSize`；模拟器日志显示那一帧约 2500 次绘制、5 批。`GPUCMD_Split` 只前移指针不扩容，所以缓冲改为 **4 MB**，C2D 批量 512 → 4096 |
| 语言名读不出 | 语言选择乱码 | `schinese.ini` 带 UTF-8 BOM，`ini.c` 跳过 BOM |

## 5. 明确没改的部分

- 游戏逻辑、存档、渲染管线主体、暂停页自己的地图版式；
- 输入层：`src/n3ds/n3ds_input.c` 与上游**逐字节相同**（曾试过用 SELECT 打开道具页，因为要在输入阶段跑 GML
  导致闪退，已整体移除，连同辅助函数与请求队列）；
- 文字位置：除 §4 的道具名 2px 外没有其它微调；旧分支另外三处（保存槽游玩时间、通知第二行、选项页提示行）
  保持游戏原样；
- 上游的 `START+SELECT` 调试面板。

## 6. 真机验证记录

| 版本 | 验证内容 |
|---|---|
| b546 | 文字恢复、中文 logo 正常 |
| b551 | 区域标题 A1–A5、A7、A8 中文；A0/A6 游戏本身没有 `sIntro_A0/A6` 素材，不播 |
| b562 | 中文界面文字完整、道具页可用 |
| v0.4.4 | 实机 area 7 按 START 打开暂停屏不再崩溃（4 MB 命令缓冲） |
| v0.4.5 | 道具页道具名下移 2px |

反馈问题时请附 `sdmc:/3ds/am2r/log.txt`；崩溃会在 `/luma/dumps/` 生成 `crash_dump_*.dmp`，用本仓库的方法
（`docs/changes-vs-upstream.md` 第 4 节 + Luma 解析规则）可以把 `pc/lr` 和栈上的返回地址映射回函数。
