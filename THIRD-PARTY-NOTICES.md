# 第三方组件声明

本仓库分发的内容里包含第三方作品。**再分发（包括打包进自己的安装程序、上传网盘、随 U 盘传播）时，下列版权声明和许可证原文必须一并保留。**

---

## 1. dsh-pet（渲染端 + 全部动画素材）

| 项 | 内容 |
|---|---|
| 名称 | dsh-pet |
| 版本 | 0.3.7 |
| 作者 | PC2005-cloud |
| 许可 | MIT License |
| 版权 | Copyright (c) 2026 PC2005-cloud |
| 来源 | npm registry `dsh-pet` |
| 在分发包中的位置 | `dsh-pet/`（含 `LICENSE`、`assets/webm/` 106 段动画） |

许可证原文见分发包内 `dsh-pet/LICENSE`。

**这一部分不是本仓库作者的成果。** 本项目只做「把它从 DSH 里独立出来运行」的宿主改造，没有修改渲染逻辑和美术资源。如果你要商用或二次创作，请联系原作者。

---

## 2. Electron

| 项 | 内容 |
|---|---|
| 名称 | Electron |
| 版本 | 43.3.0 |
| 许可 | MIT License |
| 版权 | Copyright (c) Electron contributors<br>Copyright (c) 2013-2020 GitHub Inc. |
| 在分发包中的位置 | `electron/`（含 `LICENSE`、`LICENSES.chromium.html`） |

---

## 3. Chromium

Electron 内嵌 Chromium，其许可为 BSD-3-Clause 及一系列第三方许可，**完整清单**见分发包内 `electron/LICENSES.chromium.html`（约 20 MB，请勿删除）。

---

## 4. 字体

`dsh-pet/assets/fonts/` 内含渲染端使用的字体文件，其授权条款由各字体原作者提供，随 `dsh-pet` 包一同分发。若需商用，请自行确认字体授权。

---

## 合规提示

如果你的使用场景是「公司内部分发」或「随硬件预装」，MIT 许可本身允许，但要求：

1. 保留上述所有版权声明（本文件 + 各组件 `LICENSE` 原文）
2. 不得使用原作者的名义为你的分发版本背书
3. 修改过的部分要在文档里注明「已被修改」

本项目作者不对第三方的授权状态作任何担保。**授权有疑问时，以原作者的最新声明为准。**
