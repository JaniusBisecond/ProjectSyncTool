#include "CliRunner.h"

#include "core/DiffEngine.h"
#include "core/StringUtil.h"
#include "core/SyncConfig.h"
#include "core/SyncController.h"
#include "core/SyncTypes.h"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
constexpr DWORD kPollIntervalMs = 50;
constexpr DWORD kTimeoutMs = 30 * 60 * 1000;	// 单次操作最长等待 30 分钟

struct CliOptions
{
	bool Scan = false;
	bool Execute = false;
	bool Force = false;
	SyncDirection Direction = SyncDirection::AToB;
};

CliOptions ParseOptions()
{
	CliOptions options;

	int argc = 0;
	LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	if (argv == nullptr)
	{
		return options;
	}

	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg(argv[i]);
		if (IEquals(arg, L"--cli-scan"))
		{
			options.Scan = true;
		}
		else if (IEquals(arg, L"--cli-execute"))
		{
			options.Execute = true;
		}
		else if (IEquals(arg, L"--force"))
		{
			options.Force = true;
		}
		else if (IEquals(arg, L"--pull") || IEquals(arg, L"--b2a"))
		{
			options.Direction = SyncDirection::BToA;
		}
		else if (IEquals(arg, L"--push") || IEquals(arg, L"--a2b"))
		{
			options.Direction = SyncDirection::AToB;
		}
	}

	::LocalFree(argv);
	return options;
}

// 等待控制器回到空闲状态；超时返回 false
bool WaitForIdle(SyncController& controller, SyncStage& outStage)
{
	const DWORD start = ::GetTickCount();
	for (;;)
	{
		const SyncSnapshot snapshot = controller.GetSnapshot();
		if (!snapshot.Busy())
		{
			outStage = snapshot.Stage;
			return true;
		}

		if (::GetTickCount() - start > kTimeoutMs)
		{
			return false;
		}
		::Sleep(kPollIntervalMs);
	}
}

void WriteLine(std::ofstream& file, const std::wstring& line)
{
	const std::string utf8 = ToUtf8(line) + "\r\n";
	file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void WritePhase(std::ofstream& file, const wchar_t* title, const PhaseReport& report)
{
	WriteLine(file, FormatW(L"%s: ok=%llu total=%llu removedDirs=%llu failed=%llu",
	                        title,
	                        static_cast<unsigned long long>(report.Ok),
	                        static_cast<unsigned long long>(report.Total),
	                        static_cast<unsigned long long>(report.RemovedDirs),
	                        static_cast<unsigned long long>(report.Errors.size())));
	if (!report.BackupRoot.empty())
	{
		WriteLine(file, L"backupRoot=" + report.BackupRoot);
	}
	for (const std::wstring& error : report.Errors)
	{
		WriteLine(file, L"  [失败] " + error);
	}
}

// 列出一组差异项；prefix 为空时输出与旧版一致的 [新增]/[覆盖]/[删除] 前缀，
// 非空时输出 [计划-新增] / [剩余-删除] 这类带区分的行
void WriteCopyItems(std::ofstream& file, const wchar_t* prefix, const std::vector<DiffItem>& items)
{
	const bool hasPrefix = (prefix != nullptr && prefix[0] != L'\0');
	for (const DiffItem& item : items)
	{
		const wchar_t* kind = (item.Kind == ChangeKind::New) ? L"新增" : L"覆盖";
		WriteLine(file, hasPrefix
			? (FormatW(L"[%s-%s] ", prefix, kind) + item.Rel)
			: (std::wstring(L"[") + kind + L"] " + item.Rel));
	}
}

void WriteDeleteItems(std::ofstream& file, const wchar_t* prefix, const std::vector<DiffItem>& items)
{
	const bool hasPrefix = (prefix != nullptr && prefix[0] != L'\0');
	for (const DiffItem& item : items)
	{
		WriteLine(file, hasPrefix
			? (FormatW(L"[%s-删除] ", prefix) + item.Rel)
			: (std::wstring(L"[删除] ") + item.Rel));
	}
}
}	// namespace

namespace CliRunner
{
int RunIfRequested()
{
	const CliOptions options = ParseOptions();
	if (!options.Scan && !options.Execute)
	{
		return -1;
	}

	// 以当前工作目录为基准：可以从任何位置运行
	std::error_code ec;
	std::wstring baseDir = TrimTrailingSlash(std::filesystem::current_path(ec).wstring());
	if (ec || baseDir.empty())
	{
		baseDir = GetExeDirectory();
	}

	const std::wstring configPath = JoinPath(baseDir, L"sync.ini");
	const std::wstring logDir = JoinPath(baseDir, L"logs");

	SyncConfig config = SyncConfig::Defaults();
	config.Load(configPath);

	SyncController controller;
	controller.SetConfig(config);
	controller.OpenLog(logDir);

	const DirectionTargets targets = ResolveDirection(config, options.Direction);

	// 与 GUI 完全相同的路径：控制器 -> worker 线程 -> 扫描/比对/执行。
	// 批处理模式下等价于「全部勾选」，即整树镜像。
	SyncStage stage = SyncStage::Idle;
	controller.StartScan(options.Direction, !options.Execute);
	if (!WaitForIdle(controller, stage))
	{
		controller.Cancel();
		return 1;
	}

	SyncSnapshot snapshot = controller.GetSnapshot();
	if (options.Execute && snapshot.Stage != SyncStage::Failed)
	{
		controller.StartExecute(false);
		if (!WaitForIdle(controller, stage))
		{
			controller.Cancel();
			return 1;
		}

		snapshot = controller.GetSnapshot();
		if (snapshot.Stage == SyncStage::Blocked && options.Force)
		{
			// 运行前检查未通过：默认不执行，加 --force 才继续
			controller.StartExecute(true);
			if (!WaitForIdle(controller, stage))
			{
				controller.Cancel();
				return 1;
			}
			snapshot = controller.GetSnapshot();
		}
	}

	snapshot = controller.GetSnapshot();

	std::error_code reportEc;
	std::filesystem::create_directories(std::filesystem::path(logDir), reportEc);
	const std::wstring reportName = std::wstring(options.Execute ? L"execute-report-" : L"scan-report-")
		+ TimeStamp() + L".txt";
	const std::wstring reportPath = JoinPath(logDir, reportName);

	std::ofstream report(std::filesystem::path(reportPath), std::ios::binary | std::ios::trunc);
	if (!report.is_open())
	{
		return 1;
	}

	const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
	report.write(reinterpret_cast<const char*>(bom), sizeof(bom));

	WriteLine(report, std::wstring(L"mode=") + (options.Execute ? L"execute" : L"scan")
		+ (options.Direction == SyncDirection::AToB ? L" direction=a-to-b" : L" direction=b-to-a"));
	WriteLine(report, L"folderA=" + config.RootA);
	WriteLine(report, L"folderB=" + config.RootB);
	WriteLine(report, L"backupDir=" + config.BackupDir);
	WriteLine(report, L"src=" + targets.Src);
	WriteLine(report, L"dst=" + targets.Dst);
	WriteLine(report, L"stage=" + FormatW(L"%d", static_cast<int>(snapshot.Stage)));
	WriteLine(report, L"same=" + FormatW(L"%llu", static_cast<unsigned long long>(snapshot.Diff.Same)));
	WriteLine(report, L"copy=" + FormatW(L"%llu", static_cast<unsigned long long>(snapshot.Diff.ToCopy.size())));
	WriteLine(report, L"delete=" + FormatW(L"%llu", static_cast<unsigned long long>(snapshot.Diff.ToDelete.size())));
	WriteLine(report, L"copyBytes=" + FormatW(L"%llu", static_cast<unsigned long long>(snapshot.Diff.CopyBytes)));
	WriteLine(report, L"deleteBytes=" + FormatW(L"%llu", static_cast<unsigned long long>(snapshot.Diff.DeleteBytes)));
	if (!snapshot.Message.empty())
	{
		WriteLine(report, L"message=" + snapshot.Message);
	}

	if (snapshot.Stage == SyncStage::Blocked)
	{
		WriteLine(report, L"");
		WriteLine(report, L"--- 运行前检查未通过 ---");
		for (const std::wstring& issue : snapshot.PreflightIssues)
		{
			WriteLine(report, L"[阻止] " + issue);
		}
	}

	if (options.Execute)
	{
		WriteLine(report, L"");
		WriteLine(report, L"--- 本次执行计划 ---");
		WriteLine(report, L"planned=" + FormatW(L"%llu",
			static_cast<unsigned long long>(snapshot.Plan.ToCopy.size() + snapshot.Plan.ToDelete.size())));
		WritePhase(report, L"copyPhase", snapshot.Copy);
		WritePhase(report, L"backupPhase", snapshot.Backup);
		WritePhase(report, L"deletePhase", snapshot.Delete);
		WriteLine(report, FormatW(L"verify: hasVerify=%d pass=%d",
		                          snapshot.HasVerify ? 1 : 0, snapshot.VerifyPass ? 1 : 0));

		WriteLine(report, L"");
		WriteLine(report, L"--- 计划项 ---");
		WriteCopyItems(report, L"计划", snapshot.Plan.ToCopy);
		WriteDeleteItems(report, L"计划", snapshot.Plan.ToDelete);

		WriteLine(report, L"");
		WriteLine(report, L"--- 剩余差异 ---");
		WriteCopyItems(report, L"剩余", snapshot.Diff.ToCopy);
		WriteDeleteItems(report, L"剩余", snapshot.Diff.ToDelete);
	}
	else
	{
		WriteLine(report, L"");
		WriteLine(report, L"--- COPY (新增/覆盖) ---");
		WriteCopyItems(report, L"", snapshot.Diff.ToCopy);

		WriteLine(report, L"");
		WriteLine(report, L"--- DELETE ---");
		WriteDeleteItems(report, L"", snapshot.Diff.ToDelete);
	}

	report.flush();
	report.close();

	const int exitCode = (snapshot.Stage == SyncStage::Failed || snapshot.Stage == SyncStage::Blocked) ? 1 : 0;
	controller.Shutdown();
	return exitCode;
}
}	// namespace CliRunner
