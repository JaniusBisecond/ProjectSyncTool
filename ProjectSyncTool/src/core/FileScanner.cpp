#include "core/FileScanner.h"

#include "core/StringUtil.h"

#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
// 扫描过程中汇报进度的间隔（文件数）
constexpr uint64_t kProgressInterval = 256;
}	// namespace

FileScanner::FileScanner(const SyncConfig& config) : config_(config)
{
	std::error_code ec;
	fs::path resolved = fs::weakly_canonical(fs::path(config.BackupDir), ec);
	if (ec)
	{
		ec.clear();
		resolved = fs::absolute(fs::path(config.BackupDir), ec);
	}
	if (ec || resolved.empty())
	{
		return;
	}

	backupRootKey_ = ToLowerInvariant(TrimTrailingSlash(resolved.wstring())) + L"\\";
}

bool FileScanner::IsBackupTree(const std::wstring& fullLower) const
{
	if (backupRootKey_.empty())
	{
		return false;
	}

	// 统一补一个结尾分隔符后做前缀判断，避免同级同名前缀误判
	const std::wstring key = fullLower + L"\\";
	return key.rfind(backupRootKey_, 0) == 0;
}

FileMap FileScanner::Scan(const std::wstring& root, IProgressSink* sink) const
{
	FileMap map;

	std::error_code ec;
	fs::path rootPath = fs::weakly_canonical(fs::path(root), ec);
	if (ec)
	{
		ec.clear();
		rootPath = fs::absolute(fs::path(root), ec);
		if (ec)
		{
			return map;
		}
	}

	const std::wstring rootText = TrimTrailingSlash(rootPath.wstring());
	if (rootText.empty())
	{
		return map;
	}
	const size_t rootLength = rootText.size() + 1;	// 含分隔符

	std::vector<std::wstring> stack;
	stack.push_back(rootText);

	// 已访问目录（规范化后），防止 junction / 符号链接造成环
	std::unordered_set<std::wstring> visited;
	visited.insert(ToLowerInvariant(rootText));

	uint64_t fileCount = 0;
	while (!stack.empty())
	{
		if (sink != nullptr && sink->IsCancelled())
		{
			break;
		}

		const std::wstring dir = stack.back();
		stack.pop_back();
		const bool isRoot = (dir.size() == rootText.size());

		fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
		if (ec)
		{
			ec.clear();
			continue;	// 目录读不了就跳过，等价于 PS 版的 catch { continue }
		}

		const fs::directory_iterator end;
		while (it != end)
		{
			const fs::directory_entry& entry = *it;
			const std::wstring full = entry.path().wstring();
			const std::wstring name = entry.path().filename().wstring();
			const std::wstring rel = (full.size() > rootLength) ? full.substr(rootLength) : name;

			std::error_code entryEc;
			const bool isDir = entry.is_directory(entryEc);
			entryEc.clear();

			if (isDir)
			{
				if (!IsExcluded(rel, name, true, isRoot))
				{
					const std::wstring key = ToLowerInvariant(full);
					// 备份目录整体跳过，且顺带防环
					if (!IsBackupTree(key) && visited.insert(key).second)
					{
						stack.push_back(full);
					}
				}
			}
			else if (!IsExcluded(rel, name, false, isRoot))
			{
				const uint64_t size = entry.file_size(entryEc);
				if (!entryEc)
				{
					FileEntry fileEntry;
					fileEntry.Rel = rel;
					fileEntry.Size = size;
					map[ToLowerInvariant(rel)] = std::move(fileEntry);

					++fileCount;
					if (sink != nullptr && (fileCount % kProgressInterval) == 0)
					{
						sink->OnProgress(L"扫描", fileCount, 0, rel);
					}
				}
			}

			it.increment(ec);
			if (ec)
			{
				ec.clear();
				break;	// 中途出错则放弃该目录剩余项
			}
		}
	}

	return map;
}

bool FileScanner::IsExcluded(const std::wstring& rel, const std::wstring& name, bool isDir, bool isAtRoot) const
{
	// 名字规则：文件与文件夹同一套（名字相等即排除）
	for (const std::wstring& item : config_.ExcludeAnyName)
	{
		if (IEquals(name, item))
		{
			return true;
		}
	}

	if (isAtRoot)
	{
		for (const std::wstring& item : config_.ExcludeRootName)
		{
			if (IEquals(name, item))
			{
				return true;
			}
		}
	}

	if (!isDir)
	{
		if (!isAtRoot)
		{
			return false;
		}

		// 仅顶层排扩展名
		const std::wstring lowerName = ToLowerInvariant(name);
		const size_t dot = lowerName.rfind(L'.');
		if (dot == std::wstring::npos)
		{
			return false;
		}

		const std::wstring ext = lowerName.substr(dot);
		for (const std::wstring& item : config_.ExcludeRootExt)
		{
			if (IEquals(ext, item))
			{
				return true;
			}
		}
		return false;
	}

	// 目录还要看相对路径前缀
	const std::wstring lower = ToLowerInvariant(rel);
	for (const std::wstring& prefix : config_.ExcludePathPrefix)
	{
		if (prefix.empty() || !StartsWith(lower, prefix))
		{
			continue;
		}

		// 前缀后必须是路径边界，避免 intermediate_old 被误判为 intermediate
		if (lower.size() == prefix.size())
		{
			return true;
		}

		const wchar_t next = lower[prefix.size()];
		if (next == L'\\' || next == L'/')
		{
			return true;
		}
	}
	return false;
}
