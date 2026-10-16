#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 同步方向：源（权威侧）-> 目标（被改写侧）。
// 两侧只是「任意两个文件夹」，方向由用户选择。
enum class SyncDirection
{
	AToB,	// A 侧作为源
	BToA	// B 侧作为源
};

enum class ChangeKind
{
	New,		// 目标侧不存在
	Modified	// 目标侧存在但内容不同
};

// 扫描得到的一个文件
struct FileEntry
{
	std::wstring Rel;		// 相对项目根的路径（保留原始大小写）
	uint64_t Size = 0;
};

// key = 小写相对路径（Windows 文件系统大小写不敏感）
using FileMap = std::unordered_map<std::wstring, FileEntry>;

// 一条差异
struct DiffItem
{
	std::wstring Rel;
	ChangeKind Kind = ChangeKind::Modified;
	uint64_t Size = 0;
	bool Selected = true;	// 界面上是否勾选；只有勾选项会被同步
};

// 一次比对的结果
struct DiffResult
{
	std::vector<DiffItem> ToCopy;
	std::vector<DiffItem> ToDelete;
	uint64_t Same = 0;
	uint64_t CopyBytes = 0;
	uint64_t DeleteBytes = 0;

	bool Empty() const { return ToCopy.empty() && ToDelete.empty(); }

	void Clear()
	{
		ToCopy.clear();
		ToDelete.clear();
		Same = 0;
		CopyBytes = 0;
		DeleteBytes = 0;
	}
};

// 一个执行阶段（复制 / 备份 / 删除）的结果
struct PhaseReport
{
	uint64_t Ok = 0;
	uint64_t Total = 0;
	uint64_t RemovedDirs = 0;	// 仅删除阶段使用
	std::wstring BackupRoot;	// 仅备份阶段使用
	std::vector<std::wstring> Errors;
};
