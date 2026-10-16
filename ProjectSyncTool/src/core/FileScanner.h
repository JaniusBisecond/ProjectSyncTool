#pragma once

#include "core/Progress.h"
#include "core/SyncConfig.h"
#include "core/SyncTypes.h"

// 扫描一棵项目树，按配置的排除规则产出「同步集合」。
// 语义与旧 PowerShell 版 Invoke-Scan 一致：
//   - 显式栈迭代 DFS，不受目录深度限制
//   - 备份目录自身必须排除，否则复核阶段永远无法通过
//   - 只收集文件；空目录不参与同步
class FileScanner
{
public:
	explicit FileScanner(const SyncConfig& config);

	FileMap Scan(const std::wstring& root, IProgressSink* sink) const;

private:
	bool IsExcluded(const std::wstring& rel, const std::wstring& name, bool isDir, bool isAtRoot) const;

	// 是否就是备份目录本身或位于其内部。
	// 备份目录现在由用户指定，可能被设在两侧文件夹内部，必须按绝对路径排除，
	// 否则旧备份会被当成「目标侧多余内容」参与同步甚至被删除。
	bool IsBackupTree(const std::wstring& fullLower) const;

	const SyncConfig& config_;
	std::wstring backupRootKey_;	// 小写、规范化、带结尾分隔符
};
