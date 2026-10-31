# 发布约定

## 附件

| 文件 | 是否发布 | 说明 |
|---|---|---|
| `am2r-<版本>.cia` | ✅ | FBI 安装包 |
| `am2r-<版本>.3dsx` | ✅ | Homebrew Launcher |
| `am2r-<版本>.elf` | ❌ | 带符号的调试文件，不能安装；只在排查崩溃转储时单独提供 |
| `qr-cia-<版本>.png` | ✅ | 指向本版 `.cia` 的二维码，供 FBI 扫码安装 |

## 发布说明结构

```markdown
<!-- n3ds-build -->
## 下载
* am2r-<版本>.cia — FBI 安装：<链接>
* am2r-<版本>.3dsx — Homebrew Launcher：<链接>

## 手机扫码安装（FBI）
用 FBI 的「Remote Install → Scan QR Code」扫描下面这张图，即可直接下载并安装 CIA：
![CIA QR](<二维码资产链接>)

## 更新
（按实际提交整理的条目，不写套话）
```

## 版本号

| 情况 | 编号 |
|---|---|
| 引入上游新版本（0.3 → 0.4） | v0.4.1（第三位从 1 开始） |
| 同版本内我们自己的改动 | v0.3.2、v0.3.3 … |
| 不使用 x.y.0 | — |

历史版本已按提交时间重编号为 v0.2.1–v0.3.3（原 V1.1–V1.10）。

## 生成二维码

```bash
pip install qrcode pillow
python3 -c "
import qrcode
url='https://github.com/frilxy/AM2R_3DS_CN/releases/download/v0.3.4/am2r-v0.3.4.cia'
qrcode.make(url).save('qr-cia-v0.3.4.png')"
gh release upload <版本> qr-cia-<版本>.png
```
