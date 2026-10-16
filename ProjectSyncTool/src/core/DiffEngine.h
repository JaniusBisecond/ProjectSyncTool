#pragma once

#include "core/Progress.h"
#include "core/SyncTypes.h"

// 比对两棵树的同步集合，产出「需覆盖/新增」与「需删除」。
// 与旧 PowerShell 版 Invoke-Compare 一致：
//   - 大小不同直接判定为需覆盖
//   - 大小相同才算 SHA256，避免无谓 IO
class DiffEngine
{
public:
	DiffResult Compare(const FileMap& src, const FileMap& dst,
	                   const std::wstring& srcRoot, const std::wstring& dstRoot,
	                   IProgressSink* sink) const;
};

// 只保留勾选项（部分同步用），并重算体积
DiffResult SelectOnly(const DiffResult& diff);

// 勾选项总数（含复制与删除）
uint64_t CountSelected(const DiffResult& diff);
