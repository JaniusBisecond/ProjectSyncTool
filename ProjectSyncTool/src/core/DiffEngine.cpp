#include "core/DiffEngine.h"

#include "core/Hasher.h"
#include "core/StringUtil.h"

#include <algorithm>

namespace
{
// 比对阶段的进度汇报间隔（已哈希文件数）
constexpr uint64_t kProgressInterval = 64;

void SortByRel(std::vector<DiffItem>& items)
{
	std::sort(items.begin(), items.end(), [](const DiffItem& a, const DiffItem& b)
	{
		return a.Rel < b.Rel;
	});
}
}	// namespace

DiffResult DiffEngine::Compare(const FileMap& src, const FileMap& dst,
                               const std::wstring& srcRoot, const std::wstring& dstRoot,
                               IProgressSink* sink) const
{
	DiffResult result;
	uint64_t hashed = 0;

	for (const auto& pair : src)
	{
		if (sink != nullptr && sink->IsCancelled())
		{
			break;
		}

		const FileEntry& source = pair.second;
		const auto found = dst.find(pair.first);
		if (found == dst.end())
		{
			DiffItem item;
			item.Rel = source.Rel;
			item.Kind = ChangeKind::New;
			item.Size = source.Size;
			result.ToCopy.push_back(std::move(item));
			continue;
		}

		const FileEntry& target = found->second;
		if (source.Size != target.Size)
		{
			DiffItem item;
			item.Rel = source.Rel;
			item.Kind = ChangeKind::Modified;
			item.Size = source.Size;
			result.ToCopy.push_back(std::move(item));
			continue;
		}

		std::wstring srcHash;
		std::wstring dstHash;
		if (!Hasher::Sha256File(JoinPath(srcRoot, source.Rel), srcHash)
		    || !Hasher::Sha256File(JoinPath(dstRoot, target.Rel), dstHash)
		    || srcHash != dstHash)
		{
			// 读不出来时按「需覆盖」处理，真正的失败会在复制阶段报出来
			DiffItem item;
			item.Rel = source.Rel;
			item.Kind = ChangeKind::Modified;
			item.Size = source.Size;
			result.ToCopy.push_back(std::move(item));
			continue;
		}

		++result.Same;
		++hashed;
		if (sink != nullptr && (hashed % kProgressInterval) == 0)
		{
			sink->OnProgress(L"比对", hashed, 0, source.Rel);
		}
	}

	for (const auto& pair : dst)
	{
		if (src.find(pair.first) != src.end())
		{
			continue;
		}

		DiffItem item;
		item.Rel = pair.second.Rel;
		item.Kind = ChangeKind::Modified;
		item.Size = pair.second.Size;
		result.ToDelete.push_back(std::move(item));
	}

	SortByRel(result.ToCopy);
	SortByRel(result.ToDelete);

	for (const DiffItem& item : result.ToCopy)
	{
		result.CopyBytes += item.Size;
	}
	for (const DiffItem& item : result.ToDelete)
	{
		result.DeleteBytes += item.Size;
	}

	return result;
}

DiffResult SelectOnly(const DiffResult& diff)
{
	DiffResult selected;
	selected.Same = diff.Same;

	for (const DiffItem& item : diff.ToCopy)
	{
		if (!item.Selected)
		{
			continue;
		}
		selected.ToCopy.push_back(item);
		selected.CopyBytes += item.Size;
	}

	for (const DiffItem& item : diff.ToDelete)
	{
		if (!item.Selected)
		{
			continue;
		}
		selected.ToDelete.push_back(item);
		selected.DeleteBytes += item.Size;
	}

	return selected;
}

uint64_t CountSelected(const DiffResult& diff)
{
	uint64_t count = 0;
	for (const DiffItem& item : diff.ToCopy)
	{
		if (item.Selected)
		{
			++count;
		}
	}
	for (const DiffItem& item : diff.ToDelete)
	{
		if (item.Selected)
		{
			++count;
		}
	}
	return count;
}
