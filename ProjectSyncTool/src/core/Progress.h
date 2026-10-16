#pragma once

#include <cstdint>
#include <string>

// 长时间操作（扫描 / 比对 / 复制 / 备份 / 删除）向 UI 回报进度并查询取消。
// 核心层只认识这个接口，不依赖 ImGui。
class IProgressSink
{
public:
	virtual ~IProgressSink() = default;

	// stage: 阶段名（扫描 / 比对 / 复制 / 备份 / 删除）
	// done / total: 计数，total == 0 表示总数未知
	// current: 当前处理的相对路径
	virtual void OnProgress(const std::wstring& stage, uint64_t done, uint64_t total, const std::wstring& current) = 0;

	virtual bool IsCancelled() const = 0;
};
