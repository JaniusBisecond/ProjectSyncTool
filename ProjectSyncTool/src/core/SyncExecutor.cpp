#include "core/SyncExecutor.h"

#include "core/StringUtil.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
// 目标侧只读文件（P4 未签出、从只读介质拷贝而来）必须先解除只读，否则覆盖与删除都会失败
void ClearReadOnlyAttribute(const std::wstring& path)
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		return;
	}
	if ((attributes & FILE_ATTRIBUTE_READONLY) == 0)
	{
		return;
	}
	::SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
}

// 保持最后写入时间，便于人工核对（等价于 PS 版的 SetLastWriteTimeUtc）
void PreserveWriteTime(const std::wstring& src, const std::wstring& dst)
{
	HANDLE source = ::CreateFileW(src.c_str(), GENERIC_READ,
	                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (source == INVALID_HANDLE_VALUE)
	{
		return;
	}

	FILETIME writeTime = {};
	if (::GetFileTime(source, nullptr, nullptr, &writeTime))
	{
		HANDLE target = ::CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES,
		                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (target != INVALID_HANDLE_VALUE)
		{
			::SetFileTime(target, nullptr, nullptr, &writeTime);
			::CloseHandle(target);
		}
	}

	::CloseHandle(source);
}

bool EnsureParentDirectory(const std::wstring& filePath)
{
	const fs::path parent = fs::path(filePath).parent_path();
	if (parent.empty() || fs::exists(parent))
	{
		return true;
	}

	std::error_code ec;
	fs::create_directories(parent, ec);
	return !ec;
}

std::wstring FailureText(const std::wstring& rel, const std::wstring& reason)
{
	return rel + L"  =>  " + reason;
}
}	// namespace

PhaseReport SyncExecutor::CopyPhase(const DiffResult& diff, const std::wstring& srcRoot, const std::wstring& dstRoot, IProgressSink* sink)
{
	PhaseReport report;
	report.Total = diff.ToCopy.size();

	uint64_t index = 0;
	for (const DiffItem& item : diff.ToCopy)
	{
		if (sink != nullptr && sink->IsCancelled())
		{
			break;
		}

		++index;
		if (sink != nullptr)
		{
			sink->OnProgress(L"复制", index, report.Total, item.Rel);
		}

		const std::wstring src = JoinPath(srcRoot, item.Rel);
		const std::wstring dst = JoinPath(dstRoot, item.Rel);
		if (!EnsureParentDirectory(dst))
		{
			report.Errors.push_back(FailureText(item.Rel, L"无法创建目标目录"));
			continue;
		}

		ClearReadOnlyAttribute(dst);
		if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE))
		{
			const std::wstring reason = LastErrorMessage(::GetLastError());
			report.Errors.push_back(FailureText(item.Rel, reason));
			logger_.Log(L"copy FAILED: " + item.Rel + L" : " + reason);
			continue;
		}

		PreserveWriteTime(src, dst);
		++report.Ok;
	}

	return report;
}

PhaseReport SyncExecutor::BackupPhase(const DiffResult& diff, const std::wstring& dstRoot, const std::wstring& stamp, IProgressSink* sink)
{
	PhaseReport report;
	report.Total = diff.ToDelete.size();

	// 没有待删文件时不创建备份目录，避免留下空目录
	if (diff.ToDelete.empty())
	{
		return report;
	}

	report.BackupRoot = JoinPath(TrimTrailingSlash(config_.BackupDir), stamp);

	uint64_t index = 0;
	for (const DiffItem& item : diff.ToDelete)
	{
		if (sink != nullptr && sink->IsCancelled())
		{
			break;
		}

		++index;
		if (sink != nullptr)
		{
			sink->OnProgress(L"备份", index, report.Total, item.Rel);
		}

		const std::wstring src = JoinPath(dstRoot, item.Rel);
		const std::wstring dst = JoinPath(report.BackupRoot, item.Rel);
		if (!EnsureParentDirectory(dst))
		{
			report.Errors.push_back(FailureText(item.Rel, L"无法创建备份目录"));
			continue;
		}

		ClearReadOnlyAttribute(dst);
		if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE))
		{
			const std::wstring reason = LastErrorMessage(::GetLastError());
			report.Errors.push_back(FailureText(item.Rel, reason));
			logger_.Log(L"backup FAILED: " + item.Rel + L" : " + reason);
			continue;
		}

		++report.Ok;
	}

	// 清单：即使有文件备份失败也照写，便于人工对照
	std::error_code ec;
	fs::create_directories(fs::path(report.BackupRoot), ec);
	std::ofstream list(fs::path(JoinPath(report.BackupRoot, L"_deleted-files.txt")), std::ios::binary | std::ios::trunc);
	if (list.is_open())
	{
		const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
		list.write(reinterpret_cast<const char*>(bom), sizeof(bom));

		auto writeLine = [&list](const std::wstring& line)
		{
			const std::string utf8 = ToUtf8(line) + "\r\n";
			list.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
		};

		writeLine(L"# 备份时间: " + stamp);
		writeLine(L"# 来源目录: " + dstRoot);
		writeLine(L"");
		for (const DiffItem& item : diff.ToDelete)
		{
			writeLine(item.Rel);
		}
		list.flush();
	}

	return report;
}

PhaseReport SyncExecutor::DeletePhase(const DiffResult& diff, const std::wstring& dstRoot, IProgressSink* sink)
{
	PhaseReport report;
	report.Total = diff.ToDelete.size();

	const std::wstring rootText = TrimTrailingSlash(dstRoot);
	const std::wstring backupPrefix = ToLowerInvariant(TrimTrailingSlash(config_.BackupDir) + L"\\");

	// 记录「本次有文件被删掉」的目录，用于之后回收空目录
	std::vector<std::wstring> touchedDirs;
	std::unordered_set<std::wstring> touchedKeys;

	uint64_t index = 0;
	for (const DiffItem& item : diff.ToDelete)
	{
		if (sink != nullptr && sink->IsCancelled())
		{
			break;
		}

		++index;
		if (sink != nullptr)
		{
			sink->OnProgress(L"删除", index, report.Total, item.Rel);
		}

		const std::wstring path = JoinPath(rootText, item.Rel);
		if (!fs::exists(fs::path(path)))
		{
			continue;
		}

		ClearReadOnlyAttribute(path);
		if (!::DeleteFileW(path.c_str()))
		{
			const std::wstring reason = LastErrorMessage(::GetLastError());
			report.Errors.push_back(FailureText(item.Rel, reason));
			logger_.Log(L"delete FAILED: " + item.Rel + L" : " + reason);
			continue;
		}

		++report.Ok;
		const std::wstring dir = TrimTrailingSlash(fs::path(path).parent_path().wstring());
		if (touchedKeys.insert(ToLowerInvariant(dir)).second)
		{
			touchedDirs.push_back(dir);
		}
	}

	// 回收空目录：只删「原本有文件被删掉」且「现在既无文件也无子目录」的目录
	// 只要目录下还剩任何内容（例如 .git 里的对象），就绝不删
	std::sort(touchedDirs.begin(), touchedDirs.end(), [](const std::wstring& a, const std::wstring& b)
	{
		return a.size() > b.size();
	});

	for (const std::wstring& dir : touchedDirs)
	{
		std::error_code ec;
		if (!fs::exists(fs::path(dir), ec) || ec)
		{
			continue;
		}
		if (dir.size() <= rootText.size())
		{
			continue;	// 项目根不删
		}
		if (ToLowerInvariant(dir).rfind(backupPrefix, 0) == 0)
		{
			continue;	// 备份目录内不删
		}

		fs::directory_iterator it(fs::path(dir), ec);
		if (ec)
		{
			continue;
		}
		if (it != fs::directory_iterator())
		{
			continue;	// 非空
		}

		if (fs::remove(fs::path(dir), ec) && !ec)
		{
			++report.RemovedDirs;
		}
	}

	return report;
}
