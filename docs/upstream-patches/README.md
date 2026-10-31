# 上游补丁（待移植）

从 `codeberg.org/NattensMadrigal/AM2R_3DS` 取下的原始补丁，对应 `docs/upstream-comparison.md` 的建议列表。

| 文件 | 上游提交 | 内容 | 触及文件 |
|---|---|---|---|
| `b0c705fa.patch` | b0c705fa | 暂停页按 320 排版 + 释放每进房间泄漏的左侧光照贴图 | `n3ds_am2r.c`、`n3ds_pause.c` |
| `14e9f9d7.patch` | 14e9f9d7 | 光照贴图跳过 surface 外的灯并记录耗时 | `n3ds_am2r.c` 等 |
| `e11eea27.patch` | e11eea27 | 视图 surface 每帧清理 | 渲染器 |
| `56c394b6.patch` | 56c394b6 | 只清房间外区域 | 渲染器 |
| `ddb0ae8e.patch` | ddb0ae8e | 图块使用 `tile->alpha` | 渲染器 |
| `f8f72a45.patch` | f8f72a45 | 自绘 surface 时复制一份 | 渲染器 |

## 移植注意

我们的 `src/n3ds/n3ds_am2r.c` 与上游已大幅分叉（屏幕模式、HUD 补偿、页面逻辑均为本地实现），
`b0c705fa.patch` 无法直接 `git apply`，需要按语义重写。其余渲染器补丁相对独立，可先试。

## 移植顺序建议

1. `ddb0ae8e`（图块 alpha）— 最小、独立
2. `f8f72a45`（自绘 surface 复制）— 小、独立
3. `e11eea27` + `56c394b6`（视图 surface 清理）— 一对，需一起
4. `14e9f9d7`（灯剔除）— 中等，独立
5. `b0c705fa`（暂停页 320 排版 + 光照泄漏）— 最大，需重写，放最后
