#include "core/SyncConfig.h"

#include "core/StringUtil.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
// ---- ini 读写：格式固定，够用即可，不引入第三方库 ----
//
// [Paths]
// FolderA=C:\...\YuSha
// FolderB=C:\...\YuShaU
// BackupDir=C:\...\_sync-backup
//
// [ExcludeAnyName]
// .git
// ...
//
// 列表项为「一行一项」，以 # 或 ; 开头视为注释。
// 旧版本的 [ExcludeAnyDir] / [ExcludeRootDir] / [ExcludeRootFile] 段、
// Left= / Right= / YuSha= / YuShaU= 键，以及 BackupDirName=（目标根下的目录名）
// 仍然兼容读取。

std::vector<std::wstring> SplitLines(const std::string& text)
{
	std::vector<std::wstring> lines;
	std::istringstream stream(text);
	std::string raw;
	while (std::getline(stream, raw))
	{
		if (!raw.empty() && raw.back() == '\r')
		{
			raw.pop_back();
		}

		std::wstring line = FromUtf8(raw);
		const size_t begin = line.find_first_not_of(L" \t");
		if (begin == std::wstring::npos)
		{
			lines.push_back(std::wstring());
			continue;
		}
		const size_t end = line.find_last_not_of(L" \t");
		lines.push_back(line.substr(begin, end - begin + 1));
	}
	return lines;
}

bool IsComment(const std::wstring& line)
{
	return !line.empty() && (line[0] == L'#' || line[0] == L';');
}

// 扩展名统一成「小写 + 前导点」，允许用户只写 sln
std::wstring NormalizeExtension(const std::wstring& value)
{
	std::wstring lower = ToLowerInvariant(value);
	if (lower.empty())
	{
		return lower;
	}
	if (lower.front() != L'.')
	{
		lower.insert(lower.begin(), L'.');
	}
	return lower;
}

// 规范化路径并转小写，用于比较两个目录是否相同
std::wstring CanonicalKey(const std::wstring& path, bool& ok)
{
	ok = false;
	if (path.empty())
	{
		return std::wstring();
	}

	std::error_code ec;
	std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
	if (ec)
	{
		ec.clear();
		resolved = std::filesystem::absolute(std::filesystem::path(path), ec);
		if (ec)
		{
			return std::wstring();
		}
	}

	ok = true;
	return ToLowerInvariant(TrimTrailingSlash(resolved.wstring()));
}
}	// namespace

std::wstring SyncConfig::DefaultBackupDir()
{
	return JoinPath(GetExeDirectory(), L"_sync-backup");
}

SyncConfig SyncConfig::Defaults()
{
	// 路径与排除规则默认留空；备份目录默认放在程序同级目录下，
	// 避免默认就把备份写进被同步的文件夹里。
	SyncConfig config;
	config.BackupDir = DefaultBackupDir();
	return config;
}

bool SyncConfig::Load(const std::wstring& iniPath)
{
	std::ifstream file(std::filesystem::path(iniPath), std::ios::binary);
	if (!file.is_open())
	{
		*this = Defaults();
		Save(iniPath);
		return true;
	}

	std::ostringstream buffer;
	buffer << file.rdbuf();
	std::string text = buffer.str();
	file.close();

	// 去掉 UTF-8 BOM
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
	    && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
	{
		text.erase(0, 3);
	}

	SyncConfig loaded = Defaults();
	loaded.ExcludeAnyName.clear();
	loaded.ExcludeRootName.clear();
	loaded.ExcludePathPrefix.clear();
	loaded.ExcludeRootExt.clear();

	std::wstring section;
	for (const std::wstring& line : SplitLines(text))
	{
		if (line.empty() || IsComment(line))
		{
			continue;
		}

		if (line.front() == L'[' && line.back() == L']')
		{
			section = ToLowerInvariant(line.substr(1, line.size() - 2));
			continue;
		}

		if (section == L"paths")
		{
			const size_t eq = line.find(L'=');
			if (eq == std::wstring::npos)
			{
				continue;
			}

			const std::wstring key = ToLowerInvariant(line.substr(0, eq));
			const std::wstring value = line.substr(eq + 1);
			if (key == L"foldera" || key == L"left" || key == L"yusha")
			{
				loaded.RootA = TrimTrailingSlash(value);
			}
			else if (key == L"folderb" || key == L"right" || key == L"yushau")
			{
				loaded.RootB = TrimTrailingSlash(value);
			}
			else if (key == L"backupdir")
			{
				loaded.BackupDir = TrimTrailingSlash(value);
			}
			else if (key == L"backupdirname")
			{
				// 旧版是「目标根下的目录名」，现在备份目录独立：
				// 按程序同级目录来解释这个旧值
				const std::wstring name = TrimTrailingSlash(value);
				if (!name.empty())
				{
					loaded.BackupDir = JoinPath(GetExeDirectory(), name);
				}
			}
			continue;
		}

		// 任意层级排除的文件/文件夹（含旧版的「任意层级目录名」）
		if (section == L"excludeanyname" || section == L"excludeanydir")
		{
			loaded.ExcludeAnyName.push_back(line);
			continue;
		}

		// 仅顶层排除的文件/文件夹（含旧版的「仅根目录名」与「仅根文件名」）
		if (section == L"excluderootname" || section == L"excluderootdir" || section == L"excluderootfile")
		{
			loaded.ExcludeRootName.push_back(line);
			continue;
		}

		if (section == L"excludepathprefix")
		{
			loaded.ExcludePathPrefix.push_back(ToLowerInvariant(line));
			continue;
		}

		if (section == L"excluderootext")
		{
			loaded.ExcludeRootExt.push_back(NormalizeExtension(line));
			continue;
		}
	}

	loaded.RootA = TrimTrailingSlash(loaded.RootA);
	loaded.RootB = TrimTrailingSlash(loaded.RootB);
	loaded.BackupDir = TrimTrailingSlash(loaded.BackupDir);
	if (loaded.BackupDir.empty())
	{
		loaded.BackupDir = DefaultBackupDir();
	}

	*this = loaded;
	return true;
}

bool SyncConfig::Save(const std::wstring& iniPath) const
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(iniPath).parent_path(), ec);

	std::ofstream file(std::filesystem::path(iniPath), std::ios::binary | std::ios::trunc);
	if (!file.is_open())
	{
		return false;
	}

	const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
	file.write(reinterpret_cast<const char*>(bom), sizeof(bom));

	auto writeLine = [&file](const std::wstring& line)
	{
		const std::string utf8 = ToUtf8(line) + "\r\n";
		file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
	};

	auto writeList = [&writeLine](const wchar_t* section, const std::vector<std::wstring>& items)
	{
		writeLine(std::wstring(L"[") + section + L"]");
		for (const std::wstring& item : items)
		{
			writeLine(item);
		}
		writeLine(L"");
	};

	writeLine(L"# ProjectSync 配置（可在界面里编辑并保存，也可手工修改）");
	writeLine(L"# 两个文件夹的名称、方向都不写死；路径结尾不要带反斜杠");
	writeLine(L"");

	writeLine(L"[Paths]");
	writeLine(L"FolderA=" + RootA);
	writeLine(L"FolderB=" + RootB);
	writeLine(L"BackupDir=" + BackupDir);
	writeLine(L"");

	writeList(L"ExcludeAnyName", ExcludeAnyName);
	writeList(L"ExcludeRootName", ExcludeRootName);
	writeList(L"ExcludePathPrefix", ExcludePathPrefix);
	writeList(L"ExcludeRootExt", ExcludeRootExt);

	file.flush();
	return file.good();
}

std::vector<std::wstring> SyncConfig::Validate() const
{
	std::vector<std::wstring> problems;

	if (RootA.empty())
	{
		problems.push_back(L"文件夹 A 未设置");
	}
	else if (!std::filesystem::exists(std::filesystem::path(RootA)))
	{
		problems.push_back(L"文件夹不存在: " + RootA);
	}

	if (RootB.empty())
	{
		problems.push_back(L"文件夹 B 未设置");
	}
	else if (!std::filesystem::exists(std::filesystem::path(RootB)))
	{
		problems.push_back(L"文件夹不存在: " + RootB);
	}

	if (!RootA.empty() && IEquals(RootA, RootB))
	{
		problems.push_back(L"两个文件夹指向同一个目录");
	}

	if (BackupDir.empty())
	{
		problems.push_back(L"备份目录未设置");
		return problems;
	}

	const std::filesystem::path backupPath(BackupDir);
	if (!backupPath.is_absolute())
	{
		problems.push_back(L"备份目录必须是完整路径: " + BackupDir);
		return problems;
	}
	if (backupPath.filename().empty())
	{
		problems.push_back(L"备份目录不能是磁盘根目录: " + BackupDir);
		return problems;
	}

	bool backupOk = false;
	const std::wstring backupKey = CanonicalKey(BackupDir, backupOk);
	if (backupOk)
	{
		bool rootOk = false;
		const std::wstring rootAKey = CanonicalKey(RootA, rootOk);
		if (rootOk && rootAKey == backupKey)
		{
			problems.push_back(L"备份目录不能与文件夹 A 相同");
		}

		rootOk = false;
		const std::wstring rootBKey = CanonicalKey(RootB, rootOk);
		if (rootOk && rootBKey == backupKey)
		{
			problems.push_back(L"备份目录不能与文件夹 B 相同");
		}
	}

	return problems;
}

bool SyncConfig::HasNoExcludeRules() const
{
	return ExcludeAnyName.empty() && ExcludeRootName.empty()
		&& ExcludePathPrefix.empty() && ExcludeRootExt.empty();
}

std::wstring LeafName(const std::wstring& path, const std::wstring& fallback)
{
	const std::wstring trimmed = TrimTrailingSlash(path);
	if (trimmed.empty())
	{
		return fallback;
	}

	const size_t pos = trimmed.find_last_of(L"\\/");
	const std::wstring leaf = (pos == std::wstring::npos) ? trimmed : trimmed.substr(pos + 1);
	return leaf.empty() ? fallback : leaf;
}

DirectionTargets ResolveDirection(const SyncConfig& config, SyncDirection direction)
{
	DirectionTargets targets;
	const std::wstring labelA = LeafName(config.RootA, L"文件夹 A");
	const std::wstring labelB = LeafName(config.RootB, L"文件夹 B");

	if (direction == SyncDirection::AToB)
	{
		targets.Src = config.RootA;
		targets.Dst = config.RootB;
		targets.SrcLabel = labelA;
		targets.DstLabel = labelB;
		return targets;
	}

	targets.Src = config.RootB;
	targets.Dst = config.RootA;
	targets.SrcLabel = labelB;
	targets.DstLabel = labelA;
	return targets;
}

std::wstring DefaultConfigPath()
{
	return JoinPath(GetExeDirectory(), L"sync.ini");
}

std::wstring DefaultSettingsPath()
{
	return JoinPath(GetExeDirectory(), L"settings.ini");
}

bool LoadLastConfigPath(const std::wstring& settingsPath, std::wstring& outConfigPath)
{
	outConfigPath.clear();

	std::ifstream file(std::filesystem::path(settingsPath), std::ios::binary);
	if (!file.is_open())
	{
		return false;
	}

	std::ostringstream buffer;
	buffer << file.rdbuf();
	std::string text = buffer.str();
	file.close();

	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
	    && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
	{
		text.erase(0, 3);
	}

	for (const std::wstring& line : SplitLines(text))
	{
		if (line.empty() || IsComment(line))
		{
			continue;
		}

		const size_t eq = line.find(L'=');
		if (eq == std::wstring::npos)
		{
			continue;
		}
		if (ToLowerInvariant(line.substr(0, eq)) != L"configpath")
		{
			continue;
		}

		outConfigPath = TrimTrailingSlash(line.substr(eq + 1));
		return !outConfigPath.empty();
	}

	return false;
}

bool SaveLastConfigPath(const std::wstring& settingsPath, const std::wstring& configPath)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(settingsPath).parent_path(), ec);

	std::ofstream file(std::filesystem::path(settingsPath), std::ios::binary | std::ios::trunc);
	if (!file.is_open())
	{
		return false;
	}

	const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
	file.write(reinterpret_cast<const char*>(bom), sizeof(bom));

	const std::string utf8 = ToUtf8(L"ConfigPath=" + configPath) + "\r\n";
	file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
	file.flush();
	return file.good();
}
