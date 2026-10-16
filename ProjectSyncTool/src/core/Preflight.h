#pragma once

#include "core/SyncConfig.h"

#include <string>
#include <vector>

namespace Preflight
{
// 运行前检查：路径存在性 + （可选）UnrealEditor 是否在运行。
// 返回问题清单，空表示通过。
std::vector<std::wstring> Check(const SyncConfig& config, bool checkEditor);

// 列出正在运行的 UnrealEditor 进程名（含 UnrealEditor-Cmd.exe）
bool IsUnrealEditorRunning(std::vector<std::wstring>& processNames);
}
