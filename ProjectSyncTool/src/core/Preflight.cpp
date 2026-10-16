#include "core/Preflight.h"

#include "core/StringUtil.h"

#include <Windows.h>
#include <tlhelp32.h>

#include <filesystem>

namespace
{
bool MatchesEditorProcess(const std::wstring& exeName)
{
	static const wchar_t* kEditorNames[] = {L"UnrealEditor.exe", L"UnrealEditor-Cmd.exe"};
	for (const wchar_t* candidate : kEditorNames)
	{
		if (IEquals(exeName, candidate))
		{
			return true;
		}
	}
	return false;
}
}	// namespace

namespace Preflight
{
bool IsUnrealEditorRunning(std::vector<std::wstring>& processNames)
{
	processNames.clear();

	HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);

	if (::Process32FirstW(snapshot, &entry))
	{
		do
		{
			const std::wstring name(entry.szExeFile);
			if (!MatchesEditorProcess(name))
			{
				continue;
			}
			processNames.push_back(FormatW(L"%s (PID %lu)", name.c_str(), entry.th32ProcessID));
		} while (::Process32NextW(snapshot, &entry));
	}

	::CloseHandle(snapshot);
	return !processNames.empty();
}

std::vector<std::wstring> Check(const SyncConfig& config, bool checkEditor)
{
	std::vector<std::wstring> problems = config.Validate();

	if (!checkEditor)
	{
		return problems;
	}

	std::vector<std::wstring> editors;
	if (IsUnrealEditorRunning(editors))
	{
		std::wstring joined;
		for (size_t i = 0; i < editors.size(); ++i)
		{
			if (i > 0)
			{
				joined += L", ";
			}
			joined += editors[i];
		}

		problems.push_back(L"检测到 UnrealEditor 正在运行（" + joined
		                   + L"）。引擎会锁定 .uasset / .dll 文件，此时同步可能失败或写出不完整文件，建议先关闭编辑器。");
	}

	return problems;
}
}	// namespace Preflight
