# 与上游的差异清单（便于跟随上游更新）

> **⚠️ 旧分支文档。** 记录的是 `main`（V1.x）分支的实现；当前正式版是 `new` 分支的 v0.4.x，差异清单请见 [changes-vs-upstream.md](changes-vs-upstream.md)。


上游：`codeberg.org/NattensMadrigal/AM2R_3DS`，比对基线 `b0c705f`（2026-10-28）。
本地树与上游共 **52 处路径差异**，按"跟随上游的难易"分三档。

## 一档：可以直接取上游版本（我们几乎没改或改动独立）

| 文件 | 差异行 | 说明 |
|---|---|---|
| `src/vm_builtins.c` | 37 | 只有少量本地补丁（下屏、文字测量） |
| `src/vm.c` | 14 | VM 值栈上限等 |
| `src/text_utils.h` | 12 | 文字测量 |
| `src/ini.c` | 少 | 本地小改 |
| `res/n3ds/app.rsf` | 少 | CIA 栈大小 |
| `tools/n3ds/make_sd.sh` | 26 | SD 打包脚本 |

**做法**：上游更新后，这些文件一般可以 `git checkout upstream -- <文件>` 后重放少量本地改动。

## 二档：中等分叉，需手工合并

| 文件 | 差异行 | 我们的改动 |
|---|---|---|
| `src/n3ds/main.c` | 364 | 启动黑屏、预热、首帧清屏、日志、FPSCR |
| `src/n3ds/n3ds_input.c` | 114 | START 交还游戏、SELECT 逻辑、组合键 |
| `src/n3ds/n3ds_log.c` | 74 | 崩溃日志保全、flush 间隔 |
| `src/n3ds/n3ds_am2r_native_hud.inc` | 82 | 下屏相关、光照剔除 |
| `src/n3ds/n3ds_renderer.h` | 66 | 接口扩展 |

## 三档：深度分叉，只按语义移植

| 文件 | 差异行 | 原因 |
|---|---|---|
| `src/runner.c` | 11073 | 运行器整体重构（房间/视图/绘制流程） |
| `src/n3ds/n3ds_am2r.c` | 1362 | 画面模式、HUD 补偿、页面系统 |
| `src/n3ds/n3ds_renderer.c` | 1345 | 渲染器（surface 池、缓存、文本路径） |
| `src/n3ds/n3ds_livemap.c` | 631 | **下屏地图（本仓库独有功能）** |
| `src/n3ds/n3ds_pause.c` | 625 | 暂停页与页面逻辑 |

**做法**：只能读上游 diff、理解意图、在本地按语义重写。**不要直接 merge**。

## 本仓库独有（上游没有）

| 路径 | 内容 |
|---|---|
| `src/n3ds/n3ds_livemap.c/.h` | 下屏实时地图、状态栏、缩放 |
| `src/n3ds/n3ds_font_replace.c/.h` | 中文字体替换与字形缓存 |
| `res/n3ds/am2r/overlay/` | 汉化语言文件 |
| `res/n3ds/am2r/bottom.png` | 下屏底图 |
| `docs/` | 本项目文档 |
| `.ci/` | 发布说明 |
| `README.en.md` | 英文说明 |

## 跟随上游更新的推荐流程

1. 取上游新提交：`git fetch` 后看 `git log upstream/main`；
2. **先看一档文件**：能直接取就直接取，风险最低；
3. **二档文件**：逐 hunk 手工合并，合完立即编译；
4. **三档文件**：只在本仓库里按语义重写，改完必须真机/模拟器验证；
5. 每完成一项出一个测试包，不要攒着一起发。

## 已知的语义冲突点

| 上游做法 | 我们的做法 | 处理 |
|---|---|---|
| 删除"画面模式"选项，只用一种 | 保留 4 种（宽屏/拉伸/点对点/2x） | **不跟**，我们的模式代码独立 |
| 暂停页保存/恢复 widescreen 状态 | 每帧重申为 0 | 等价，保留本地实现 |
| START 打开暂停菜单（含下屏点击） | START 完全交还游戏 | 保留本地行为 |
