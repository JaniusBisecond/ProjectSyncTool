#pragma once

#include "core/Logger.h"
#include "core/Progress.h"
#include "core/SyncConfig.h"
#include "core/SyncTypes.h"

// 执行同步的三个写阶段。顺序固定：先复制 -> 再备份 -> 再删除。
// 与旧 PowerShell 版 Invoke-CopyPhase / Invoke-BackupPhase / Invoke-DeletePhase 一致，
// 另加两点稳健性处理：
//   1) 目标文件是只读（P4 工作副本常态）时先清掉只读属性，否则覆盖/删除必然失败
//   2) 所有失败都收集到 PhaseReport::Errors，不中断后续文件
class SyncExecutor
{
public:
	SyncExecutor(const SyncConfig& config, Logger& logger) : config_(config), logger_(logger) {}

	// 覆盖 + 新增，保持源文件最后写入时间
	PhaseReport CopyPhase(const DiffResult& diff, const std::wstring& srcRoot, const std::wstring& dstRoot, IProgressSink* sink);

	// 把待删文件复制到 <目标根>\<备份目录名>\<stamp>\，并落 _deleted-files.txt 清单
	PhaseReport BackupPhase(const DiffResult& diff, const std::wstring& dstRoot, const std::wstring& stamp, IProgressSink* sink);

	// 删除目标侧多余文件，并回收「因本次删除而变空」的目录
	PhaseReport DeletePhase(const DiffResult& diff, const std::wstring& dstRoot, IProgressSink* sink);

private:
	const SyncConfig& config_;
	Logger& logger_;
};
