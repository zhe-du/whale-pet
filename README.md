# 鲸鱼桌宠 · Whale Desktop Pet

把一只蓝色鲸鱼娘放到 Windows 桌面上。透明置顶窗口、无边框、可拖动，106 段 webm 动画随机切换——发呆、吃零食、玩魔方、写代码、打盹，鼠标划过还会有反应。

**下载**：[Releases](../../releases) → `whale-pet-v1.1.0-win64.zip`（184 MB，解压即用，不需要安装）

![icon](whale-pet-icon.png)

---

## 这是什么

一个**独立运行的桌面宠物宿主**。

渲染端是社区开源的 [dsh-pet](https://www.npmjs.com/package/dsh-pet)（MIT），原本只能跑在 DSH（DeepSeek Harness）里面。这个项目把它剥出来做成了单机程序：自己起 HTTP 服务提供配置和素材，自己拉起 Electron 显示窗口，**不依赖 DSH、不依赖系统 Node.js、不依赖任何在线服务**。

包含两套宿主实现，默认用 Node 那套：

| 组件 | 文件 | 说明 |
|---|---|---|
| Node 宿主 | `server.mjs` | **默认**。起本地 HTTP 服务（39471），托管配置/素材/字体，拉起 Electron |
| C++ 宿主 | `bin/whale-pet-host.exe` | 备用。没装 Node 也没 Electron 时兜底，纯 Win32 + Winsock |
| 启动器 | `启动鲸鱼桌宠.vbs` | 无黑窗。设置自包含环境变量后调 `launcher.cmd` |
| 运行时 | `electron/` | Electron 43.3.0（裁剪语言包），同时也是宿主自己的 Node |

## 快速开始

1. 下载 zip，解压到任意目录（建议 `D:\Apps\whale-pet`，**不要放桌面**，会被 OneDrive 同步拖慢）
2. 双击 **`启动鲸鱼桌宠.vbs`**
3. 鲸鱼出现在桌面右上角，可以拖着走

想让它开机自己出来：双击 `设置-开机自启.vbs`（写一条 HKCU Run 项，不需要管理员）。
不想要了：双击 `取消-开机自启.vbs`，再双击 `停止鲸鱼桌宠.cmd`，最后删掉整个文件夹——没有残留。

## 系统要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 1809+ / Windows 11，**仅 x64** |
| 磁盘 | 约 400 MB（解压后 366 MB） |
| 内存 | 空闲时约 300 MB（4 个 Electron 进程） |
| 显卡 | 任意支持 DWM 合成的显卡。老驱动或远程桌面下可能白屏，见「常见问题」 |
| 其他 | 无。**不需要装 Node.js、不需要管理员权限、不需要联网** |

---

## 声明

### 用途

本程序把一只桌宠渲染在桌面最上层，仅此而已。它**不会**：

- 修改系统设置、注册表（除了你自己选择开启的那条开机自启项）
- 安装服务、驱动、浏览器插件
- 监听键盘、截屏、读取你的文档
- 提供 AI 对话能力（渲染端有 whisper 接口，但本项目默认关闭且没有配置任何模型）
- 后台常驻无窗口进程（退出后进程清零）

### 数据与隐私

**全离线运行。** 除下面这一条例外，程序不发任何网络请求：

- 渲染端内置「余额查询」功能，会向 DeepSeek / OpenCode 的 API 请求你自己的账户余额。**本项目默认没有配置任何 key**，`userdata/dsh-pet/main-config.jsonc` 里 `balanceEnabled` 需要你手动打开并自己填凭据才会生效。

它写什么：

- `<程序目录>\userdata\` —— 配置、Chromium profile
- `<程序目录>\logs\` —— 运行日志（纯文本，含 HTTP 请求路径）
- `%LOCALAPPDATA%\whale-pet-standalone\chromium\` —— Electron 的缓存目录

它**不写**：注册表（除非你装了自启）、系统目录、用户文档目录。所有数据都在程序自己的文件夹里，删文件夹即清空。

### 安全

| 项 | 实际情况 |
|---|---|
| 管理员权限 | **不需要，也不用**。全程以当前用户身份运行 |
| 网络监听 | 仅 `127.0.0.1:39471`，只监听回环，局域网访问不到 |
| 进程行为 | 自己起一个 Electron，除此之外不拉别的进程 |
| 代码注入 | 无 |
| 开机自启 | 可选。写 `HKCU\...\Run\WhaleDesktopPet`，一条字符串，用 `取消-开机自启.vbs` 一键删 |

⚠️ `bin\whale-pet-host.exe` 是本地编译的**未签名**程序。Windows 智能应用控制（Smart App Control）开启的机器上，首次运行会弹「无法验证发布者」。这是系统策略，不是病毒。签名过的 `electron.exe` 不会弹。默认启动路径不经过这个 exe。

### 兼容性

- ✅ Windows 10 1809 及以上、Windows 11（x64）
- ❌ 32 位系统、ARM64（未编译）、Linux、macOS
- ⚠️ 多显示器 / 高 DPI 已适配（按主屏缩放比自动计算窗口尺寸），但 125%/150% 混合缩放下位置可能有偏移
- ⚠️ 远程桌面、虚拟机、无 GPU 加速环境：透明窗口可能黑底，用 `--disable-gpu` 启动 Electron 可缓解

### 免责

按**现状**提供，不附带任何明示或暗示的担保。

- 本项目只是容器，**不拥有**渲染端和动画素材的版权（详见下方第三方组件）
- 从非本仓库渠道获取的压缩包可能被篡改，请用 Release 页的 SHA256 校验
- 因修改配置、二次分发、商业使用产生的后果由使用者承担
- 桌宠会一直待在桌面最上层，**做全屏演示或录屏前记得先停掉**，否则它可能入镜

### 许可

本项目自己的代码（`server.mjs` 的独立宿主逻辑、`src/` 下的 C++ 源码、所有启动脚本）采用 **MIT 许可**，见 [LICENSE](LICENSE)。

渲染端和动画素材来自第三方，版权归原作者，**再分发时必须保留其许可证**——详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

---

## 第三方组件

| 组件 | 版本 | 许可 | 版权 |
|---|---|---|---|
| [dsh-pet](https://www.npmjs.com/package/dsh-pet) | 0.3.7 | MIT | © 2026 PC2005-cloud |
| [Electron](https://www.electronjs.org/) | 43.3.0 | MIT | © Electron contributors / OpenJS Foundation |
| Chromium | 随 Electron | BSD-3-Clause 等 | © The Chromium Authors |

分发包内已包含各自的许可证原文：`dsh-pet/LICENSE`、`electron/LICENSE`、`electron/LICENSES.chromium.html`。

**如果原作者的授权范围有变化，以原作者声明为准。**

## 它怎么工作

```
启动鲸鱼桌宠.vbs
   └─ launcher.cmd            设 DSH_HOME / DSH_PET_PACKAGE_ROOT 等环境变量
        └─ electron.exe      以 ELECTRON_RUN_AS_NODE=1 运行，此时它就是个 node
             └─ server.mjs   HTTP 服务 :39471
                  ├─ /dsh-pet-7340/config      宠物配置
                  ├─ /dsh-pet-7340/thumb/...   动画素材（webm）
                  └─ spawn electron.exe        这次不带 RUN_AS_NODE，正常开窗口
                       └─ dsh-pet/runtime/electron-helper/main.js
                            └─ 透明置顶窗口 + <video> 播放动画
```

关键点：**同一个 electron.exe 当两种东西用**。第一次以 Node 模式跑宿主，第二次以正常模式开窗口。这样分发包里省掉一整个 Node 运行时。

宿主被强杀时，`DSH_PET_HOST_PID` 让 helper 在 2 秒内自己退出；Electron 5 秒内连挂 3 次则宿主熔断退出，避免无限重启。

## 从源码构建

```powershell
# 1. 准备 Electron 与 dsh-pet（从任意 DSH 安装里取，或 npm i dsh-pet）
#    目录结构见 build.py 顶部常量

# 2. 组装
python build.py

# 3. 生成启动脚本
python gen_launchers.py
```

编译 C++ 宿主（可选，仅当要替换 `bin\whale-pet-host.exe`）：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build.ps1        # 需要 MinGW GCC，路径见脚本内
```

## 常见问题

**双击没反应？**
看 `logs\standalone-<日期>.log` 最后 20 行。最常见是端口 39471 被上一个没退干净的实例占着，双击 `停止鲸鱼桌宠.cmd` 再来一次。

**杀毒软件报毒？**
`server.mjs` 会 spawn 子进程、开本地端口，启发式引擎容易误判。`whale-pet-host.exe` 未签名，也会被标「未知发布者」。可以加排除项，或者只保留 `whale-pet-host.exe` 以外的部分（默认路径用不到它）。

**桌宠是黑底/白屏？**
显卡驱动或远程桌面环境。编辑 `launcher.cmd`，在 `start "" "%BASE%\electron\electron.exe"` 后面加 `--disable-gpu`。

**能同时开两只吗？**
默认不行——宿主检测到 39471 已被应答会主动退出，防止双鲸鱼重叠。要并排跑，设不同的 `DSH_PET_STANDALONE_PORT`。

**桌宠挡住了东西？**
它就是个置顶窗口，拖到角落，或者双击 `停止鲸鱼桌宠.cmd` 关掉。
