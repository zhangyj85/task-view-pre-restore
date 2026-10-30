# Task View Pre-Restore

**任务视图预还原** · A Windhawk mod that pre-restores minimized windows before Task View switches to them.

在 Win+Tab 任务视图中选中最小化窗口时，先在后台完成无激活还原，再继续 Windows 原有的任务视图展开过渡，减少窗口还原动画与任务视图动画叠加的问题。

本项目源于 Windows 与 MyDockFinder 同时启用动画时的冲突，当前实现无需 Dock 钩子，也不依赖 MyDockFinder。**0.9.2 已在开发者测试环境中由用户确认功能正常**；其他系统构建与应用组合仍需验证。

## 功能

- 保留原有任务视图展开动画，以及窗口最小化前的最大化状态。
- 不修改 Windows 或 MyDockFinder 的全局动画设置。
- 普通窗口直接由 Explorer 预还原，无需向每个应用注入。
- 对任务管理器、MMC 等需要在进程内处理的窗口，提供用户自选的接收进程名单。
- 一份源码，同时支持 x86/x64 接收端；桌面 Shell 部分为 x64。
- 无日志线程、诊断快照、全局窗口扫描或逐帧钩子。

## 兼容性

当前适配 Windows 11 23H2 x64 测试环境，使用 Windhawk 1.7.3 验证。

**Shell 部分使用私有接口固定偏移，仅支持已核对的 `twinui.pcshell.dll` 构建：**

| 标识 | 值 |
|---|---|
| PE 时间戳 | `0xd8ceffa6` |
| 映像大小 | `0xa27000` |

初始化还会检查入口指令及虚表。构建不匹配时拒绝启用 Shell 功能；不能仅凭“Windows 11 23H2”判断兼容，也不要删除这些检查强行启用。Windows 更新改变该组件后，需要重新适配。

已测试的场景包括普通窗口、任务管理器、MMC 服务管理器，以及本机基于 VSCodium 的 Windhawk 界面。没有量化 FPS，也不保证所有应用或电源模式下表现一致。

## 安装

### 便携版 Windhawk

仓库提供的 PowerShell 脚本适用于便携版，使用其自带的编译器和 SDK，无需安装其他 C++ 运行库。

1. 下载仓库文件并放在同一目录。
2. 按需编辑 `ReceiverTargets.txt`。默认包含 `mmc.exe` 和 `Taskmgr.exe`。
3. 在仓库目录运行以下命令，将路径替换为实际便携版 Windhawk 目录：

```powershell
.\Install.ps1 -WindhawkDirectory 'D:\Apps\Windhawk'
```

仅编译、暂不安装：

```powershell
.\Install.ps1 -WindhawkDirectory 'D:\Apps\Windhawk' -BuildOnly
```

安装脚本生成两种架构的 DLL，保存已有配置备份，并将 Explorer 和指定接收进程加入所需的允许规则。若提示全局进程名单发生变化，正常退出 Windhawk，再以管理员身份启动；某些已运行的目标程序可能需要重新打开。

向高权限应用加载接收端需要 Windhawk 具有相应权限。Explorer 保持普通权限。Windhawk 安全模式会暂停注入，脚本不会自动关闭安全模式。

### 通过源码安装

也可以将 [`task-view-pre-restore.wh.cpp`](task-view-pre-restore.wh.cpp) 作为本地 Windhawk mod 编译加载。默认只包含 `explorer.exe`；如需接收端，应在该 mod 的附加进程包含规则中添加目标 EXE，并确保全局注入规则允许它们、保留默认 Explorer 包含项。`ReceiverTargets.txt` 由配套脚本读取，单独导入 C++ 源码时不会自动应用该文件。

## 选择接收进程

`ReceiverTargets.txt` 每行填写一个明确的 EXE 文件名或本地完整路径，不加引号、不使用通配符：

```text
mmc.exe
Taskmgr.exe
D:\Apps\Windhawk\UI\VSCodium.exe
```

最后一行为示例：需要处理 Windhawk 自身界面时，填写实际 UI 宿主路径，而非照抄示例。文件名规则会匹配同名进程，完整路径可以缩小匹配范围。留空则只保留 Explorer 通用路径。

修改名单后运行，无需重编译：

```powershell
.\Apply-Targets.ps1 -WindhawkDirectory 'D:\Apps\Windhawk'
```

所有指定应用使用同一套接收流程，没有针对 Taskmgr、MMC 或 Electron 的单独实现。普通应用不必加入；仅在通用路径无法处理时使用接收端。多进程程序中，匹配名单的各实例都会加载接收端。

## 工作原理

Explorer 中仅挂接任务视图的选择入口 `InvokeSwitchItem`。选中最小化窗口后，保存窗口位置，临时禁用该窗口的 DWM 过渡，通过 `ShowWindowAsync(SW_SHOWNOACTIVATE)` 无激活还原。

窗口状态就绪并稳定 32 ms 后，重新启用过渡，再调用原始 Shell 选择流程。调度在原 GUI 线程上进行，定时器仅在操作期间活动；常规准备超时为 250 ms。

本地预还原失败时，才尝试通过命名管道让目标进程自行执行相同操作。接收端校验请求来自实际桌面 Shell，并只处理本进程窗口；清除过渡禁用状态后发送完成确认。取消时尝试恢复原最小化位置。

Shell 不增加工作线程。每个接收实例有一个空闲等待线程和两个异步管道实例；没有独立辅助 EXE。32 ms 为当前已验证方案的经验稳定期，并非系统提供的画面就绪通知。

## 停用与恢复

在 Windhawk 中禁用本 mod 即可停用。

通过脚本覆盖已有版本后，可恢复保存的旧配置：

```powershell
.\Restore-Previous.ps1 -WindhawkDirectory 'D:\Apps\Windhawk'
```

按脚本提示重启 Windhawk。恢复要求备份中存在旧 mod 配置；首次安装没有旧版本可恢复。安装与名单脚本可能添加全局进程允许项，恢复脚本会保留旧配置需要的项目，不会整体覆盖其他 mod 的设置。

## 文件

| 文件 | 用途 |
|---|---|
| `task-view-pre-restore.wh.cpp` | 完整 mod 源码 |
| `ReceiverTargets.txt` | 用户自选接收进程 |
| `Install.ps1` | 编译、备份与安装 |
| `Apply-Targets.ps1` | 应用接收进程名单 |
| `Restore-Previous.ps1` | 恢复旧配置 |
| `Configuration.ps1` | 脚本共享配置逻辑 |

内部 mod ID 保留为 `taskview-restore-preparer`，便于已有本地版本原位升级。

## 问题反馈

请提供 Windows 构建、Windhawk 版本、目标 EXE、是否以管理员身份运行、最小化前是否最大化，以及冲突发生在任务视图还是 Dock。涉及兼容性时，也请提供 `twinui.pcshell.dll` 版本与上述构建标识。
