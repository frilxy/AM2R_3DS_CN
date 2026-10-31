# 中文字库：离线烘焙 + 运行时装载（新分支 new）

上游移植（Butterscotch/AM2R 3DS）的 `data.win` 里**没有任何中文字形**。AM2R 的汉化语言包靠 `font_replace` 在运行时
整字体替换，而这个内置函数上游没有实现；旧分支（v1.x）自己实现了它：开机时在 3DS 上解析 TTF 的 1-bit 点阵
（EBLC/EBDT）、打包成一张 1024×1024 贴图，再把字形表换掉。能跑，但把约 1000 行渲染器代码绑在了分叉上，
每次跟随上游都要重做。而且每次开机要解 3 种点阵 × 1288 个字形（约 1.3 s），才有了 `fontcache.bin` 那层缓存。

新分支换了个切法：**字形在 PC 上离线烘好**，3DS 只做"读包 → 传一张贴图 → 改字形指针"，对上游渲染器的改动压到
最小（新增两个小函数）。

## 组成

| 部分 | 位置 | 作用 |
|---|---|---|
| 生成器 | `tools/n3ds/fontgen/make_cjk_font.py` | 读语言文件 + TTF 的 EBLC/EBDT，烘出 `cjk-font.bin`（+ 预览图 `cjk-font.png`） |
| 产物 | `res/n3ds/am2r/overlay/lang/fonts/cjk-font.{bin,png}` | 随 overlay 进 SD 卡：`sdmc:/3ds/am2r/lang/fonts/` |
| 校验 | `tools/n3ds/fontgen/verify_cjk_font.py` | 用与 C 相同的偏移解析产物，并按绘制规则把字画出来 |
| 运行时装载 | `src/n3ds/n3ds_cjk_font.c/.h` | 读包、建贴图页、装字形 |
| 渲染器接口 | `N3DSRenderer_allocImagePage` / `_flushImagePage` / `_invalidateTextLayoutCache` | 建一张运行时贴图页（复用 surface 的 dynamic capture TPAG 通道） |
| 挂钩 | `src/n3ds/n3ds_am2r.c` 的 `font_replace` hook | 语言包调用时套用 |

## 运行时做的事（就这些）

1. 游戏在 `oControl` 的 Create 事件里对每个字体调 `font_replace(font, "lang/fonts/xxx.ttf", size, ...)`；
   hook 把它交给 `N3DSCjkFont_apply`。
2. 第一次调用时读 `lang/fonts/cjk-font.bin`（在 TTF 旁边），拿包里的 1-bit 位图建一张 1024×1024 RGBA5551 贴图，
   作为一张运行时贴图页（TPAG）交给渲染器。**不改绘制流程、不改布局缓存、不碰 surface**。
3. 之后每个字体只做一件小事：按「文件名 + 字号」在包里的条带表中找到自己那一块，把字形矩形表拷进 `Font`，
   `tpagIndex` 指向那张贴图页。

所有字体**共用一张贴图页**：一条条带是一条（字体文件内容 × 请求字号）的字形块，按高度从大到小、从上往下排。
`font_replace` 要求同一字体所有字形在一张贴图里，共用一页正好也省显存。

## 包的格式（小端）

```
0   char  magic[4]      "CJKF"
4   u32   version       1
8   u16   texWidth      1024（2 的幂，8..1024）
10  u16   texHeight     1024（2 的幂，>= 实际用到的行数）
12  u16   usedRows      有条带的行数
14  u16   stripCount
16  u32   stripTableAt
20  u32   bitmapAt
24  u32   bitmapBytes   = (texWidth/8) * usedRows
28  u32   reserved
32  strip[stripCount]   44 字节：name[32], u16 size, u16 yOffset, u16 maxGlyphHeight,
                                  u16 glyphCount, u32 glyphOffset
..  glyph[...]         14 字节：u16 codepoint, u16 x, u16 y, u16 w, u16 h, i16 shift, i16 offset
..  bitmap             每行 texWidth/8 字节，MSB 在左
```

`y` 已经是贴图页内的绝对行号（条带的 `yOffset` 加进去了），运行时不再做偏移计算。

## 字形矩形：和游戏自己的字体同一套

runner 画字时把字形矩形放在**行顶**（`cursorY`），矩形底边就是这一笔的墨脚；行距是最高矩形的行数。所以

* 矩形高 = `请求字号 - bearingY + 墨高`（请求字号就是**升部**，游戏给的字号就是这个意思）；
* 墨在矩形里从 `请求字号 - bearingY` 行开始，横向从 `|bearingX|` 处开始；
* `shift` = 步进（advance），`offset` = 游戏自己那份字体的 1px 微调（负 bearing 时矩形向左长，不再右移）。

旧运行时就是按这套规则打包的，生成器逐条复刻；超出升部的字（`bearingY > 字号`）把矩形夹到行顶，宁可低一两像素
也不丢字（旧代码会把这类字画成空白）。

## 重新生成

```
python3 tools/n3ds/fontgen/make_cjk_font.py      # 生成 cjk-font.bin + cjk-font.png
python3 tools/n3ds/fontgen/verify_cjk_font.py    # 自检，并用产物的位图把一句中文画成字符画
```

`make_cjk_font.py` 只依赖标准库（`zlib`/`struct`），不需要 Pillow，也不需要 devkitARM。烘进包里的字符是：

* `lang/languages.txt` 列出的**所有**语言文件实际用到的字（切语言不用重新生成）；
* 可打印 ASCII 与中文标点；
* `src/n3ds/*.c` 里所有含非 ASCII 的字符串字面量——移植自己画的下屏页面（显示/金手指）那几行字不在任何语言文件里，
  少一个字就是空白。

字号与字体名的对应关系来自 `sizes.ini`（游戏自己读的那份），脚本按 `oControl` 里的 `font_replace` 调用顺序
生成条带。`04b09.ttf` 会被请求 12 与 10 两个字号，条带按「文件内容 × 字号」去重，所以三个 12px 文件只占一块。

## 实测（当前产物）

| 条带（字体 × 请求字号） | 贴图行 | 字形 | 备注 |
|---|---|---|---|
| `04b09.ttf` @12 | 0–182 | 1291 | 三个 12px 文件内容相同，共用这一块 |
| `Acknowledge_TT_BRK.ttf` @12 | 同上 | 1291 | 与上一行同一块 |
| `Glasstown_NBP.ttf` @12 | 同上 | 1291 | 与上一行同一块 |
| `04b09.ttf` @10 | 0–182 | 1291 | 同一个文件的小字号，**复用 12px 的点阵**（矩形顶端裁 2 行），不另占像素 |
| `uni_05_53.ttf` @10 | 182–304 | 1291 | |
| `04b24.ttf` @10 | 304–404 | 1290 | 8px 点阵，字库缺 `╳` 一个 |

四块点阵、六个条带条目，404/512 行；`cjk-font.bin` 约 121 KB，运行时贴图 1024×512 RGBA5551（1 MB 显存）。

只烘**用得上的字**：1292 个码位 = 语言文件里出现的 1258 个 + 移植自己页面用到的 170 个（其中 8 个只有移植用：
`亮日版绘脚药隔；`）+ 可打印 ASCII 95 + 中文标点。三种 12px 字体内容完全相同，只占一块点阵；`04b09` 的 10px 请求
按升部差把矩形顶端裁掉 2 行空白复用同一批像素——1291 个字里只有一个 `╳` 的墨迹顶到行顶没法裁，它保持 12px
的矩形，比专门烘一份低 2 像素，肉眼看不出来。

## 验证

* **解析器**：生成器里的 EBLC/EBDT 解析是从旧运行的 C 实现（`n3ds_font_replace.c`，已删除）逐行移植的，
  在删除前用宿主编译的同一份 C 代码对 5 个字体 × 1285 个码位做了逐字节比对（0 差异）。
* **包**：`verify_cjk_font.py` 用与 C 相同的偏移和边界规则解析，并检查每个字形都落在自己的条带行内。
* **真机**：CI 编出的 `am2r.cia` 装上后看中文是否正常（标题、对话、道具页）。

## 限制

* 字号必须与烘焙时一致：语言包如果在 SD 上改了 `sizes.ini`，包里的字号对不上就**不套用**（保留游戏原字体，日志里
  会写 `the CJK font pack has no <font> at <n>px`）。改字号要重新跑生成器。
* 翻译改动（`schinese.ini` 增删字）同样要重新生成，否则新字是空白；这是离线烘焙的代价。
* 所有条带必须塞进同一张 1024×1024 页面（一个字体一个 `tpagIndex`）。当前用量 556 行，余量还够。
* 运行时不再有 `fontcache.bin`（没有要缓存的东西），旧版本留下的文件可以删掉。
