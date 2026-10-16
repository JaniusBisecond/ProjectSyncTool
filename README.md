# ProjectSyncTool

Windows 上「两个文件夹之间的差异同步」小工具。C++ / Win32 + Direct3D 11 + Dear ImGui，单文件 exe，无额外运行时依赖（除 VC++ 运行库）。

两侧可以是任意两个文件夹，方向可切换，同步范围由你在差异列表里勾选决定。

## 功能

- **差异对比**：按可配置的排除规则扫描两侧，用「大小 + SHA256」比对，列出需覆盖/新增与需删除的项
- **按勾选同步**：差异表每项都能勾选，只对勾选项做复制/删除；表头与工具条支持 全选 / 全不选 / 反选 / 仅新增·覆盖
- **删除前先备份**：删除项先复制到自定义备份目录下的 `<时间戳>\` 子目录，并写 `_deleted-files.txt` 清单
- **执行后复核**：重新扫描比对，只判定本次勾选项，报告是否收敛
- **可中断**：扫描 / 比对 / 复制 / 备份 / 删除过程中都能点「取消」
- **排除规则 4 类**：任意层级排除的文件/文件夹、仅顶层排除的文件/文件夹、排除的相对路径前缀、仅顶层排除的扩展名
- **配置可导入/另存**：ini 文件，位置自选，并记住上次使用的那一份
- **拖拽填路径**：文件夹 A / 文件夹 B / 备份目录三个输入框都支持从资源管理器拖入
- **日志**：写 `logs\sync-<时间戳>.log`，界面「日志」页可直接打开
- **批处理模式**：`--cli-scan` / `--cli-execute`，不开界面，便于自动化

## 构建

需要：

- Visual Studio 2022（v143 工具集）+ Windows SDK 10
- [Dear ImGui](https://github.com/ocornut/imgui) 1.92 或更新（需包含 `backends/imgui_impl_win32.*` 与 `backends/imgui_impl_dx11.*`；本项目在 1.93.1 WIP 上开发，依赖 1.92+ 的动态字体纹理）

ImGui 不随本仓库分发，请自行获取：

```
git clone https://github.com/ocornut/imgui
```

然后用环境变量指向它（工程优先读 `IMGUI_ROOT`）：

```
setx IMGUI_ROOT D:\path\to\imgui
```

设完重新打开 VS（或新开一个终端）再编译。命令行：

```
msbuild ProjectSyncTool\ProjectSyncTool.sln /p:Configuration=Release /p:Platform=x64
```

产物：`ProjectSyncTool\x64\Release\ProjectSyncTool.exe`

## 使用

1. 启动 exe。首次运行会在 exe 旁生成 `sync.ini` 与 `logs\`
2. 「配置」页填两个文件夹（可「浏览…」或从资源管理器拖入），按需修改备份目录与排除规则，点「保存配置」
3. 「同步」页选方向（`A → B` 或 `B → A`）→ 点「差异对比」→ 在列表中勾选要同步的项 → 点「执行同步」→ 确认
4. 「一致性校验」做整树只读校验；「日志」页可打开日志文件与日志目录

命令行（用于自动化，等价于全部勾选）：

```
ProjectSyncTool.exe --cli-scan   --push     # A -> B，只扫描并写报告
ProjectSyncTool.exe --cli-execute --pull --force   # B -> A，执行同步
```

报告写在 `logs\scan-report-*.txt` / `logs\execute-report-*.txt`。

## 语义说明

- 同步是**镜像**：勾选项在目标侧被源侧覆盖或新增；源侧不存在的勾选项会被删除
- 空目录不参与同步；删除后只回收「因本次删除而变空」的目录
- 目标侧的**只读文件**（例如 P4 未签出的工作副本）会先清除只读属性再覆盖/删除
- **备份目录**默认是程序同级目录下的 `_sync-backup`，可改成任意完整路径；如果它被设在 A/B 内部，会被按绝对路径整体排除，不会参与同步也不会被当成多余内容删掉
- 运行前会检查 `UnrealEditor` 是否在运行（它锁定 `.uasset`/`.dll` 时同步可能失败或写出不完整文件），检测到会提示，可选择忽略继续
- 配置里若一条排除规则都没有，界面会给出警告：`.git` / `Intermediate` / `Saved` 等都会被当作同步内容

## 项目结构

```
ProjectSyncTool/
  src/main.cpp          Win32 + D3D11 + ImGui 宿主，消息循环
  src/CliRunner.*       批处理模式
  src/ui/AppUi.*        界面层：只读快照 + 派发命令
  src/core/             核心层（不含任何 ImGui 头文件，可独立编译）
    SyncTypes.h         类型：FileEntry / DiffItem / DiffResult / PhaseReport
    SyncConfig.*        配置读写与校验
    FileScanner.*       扫描 + 排除规则 + 备份目录排除
    DiffEngine.*        比对（大小 + SHA256）与勾选过滤
    SyncExecutor.*      复制 / 备份 / 删除（含空目录回收）
    SyncController.*    状态机 + worker 线程 + 快照
    Hasher / Logger / Preflight / StringUtil
```

## 开发说明

- 核心层不依赖 ImGui；界面层每帧只读控制器快照（按 revision 决定是否重新拉取），所有耗时操作都在 worker 线程，界面不会卡死
- 界面文案与日志为中文，中文字体在启动时从 `C:\Windows\Fonts` 加载

## 许可

本仓库尚未指定开源许可。使用的 [Dear ImGui](https://github.com/ocornut/imgui) 为 MIT 许可，不随本仓库分发。
