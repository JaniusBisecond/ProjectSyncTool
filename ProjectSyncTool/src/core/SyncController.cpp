#include "core/SyncController.h"

#include "core/DiffEngine.h"
#include "core/FileScanner.h"
#include "core/Preflight.h"
#include "core/StringUtil.h"
#include "core/SyncExecutor.h"

#include <filesystem>
#include <unordered_set>

namespace
{
std::wstring JoinIssues(const std::vector<std::wstring>& issues)
{
	std::wstring joined;
	for (size_t i = 0; i < issues.size(); ++i)
	{
		if (i > 0)
		{
			joined += L"；";
		}
		joined += issues[i];
	}
	return joined;
}
}	// namespace

// 把核心层的进度回调桥接到控制器状态
class SyncController::Sink : public IProgressSink
{
public:
	explicit Sink(SyncController& owner) : owner_(owner) {}

	void OnProgress(const std::wstring& stage, uint64_t done, uint64_t total, const std::wstring& current) override
	{
		owner_.OnSinkProgress(stage, done, total, current);
	}

	bool IsCancelled() const override { return owner_.cancel_.load(); }

private:
	SyncController& owner_;
};

SyncController::~SyncController()
{
	Shutdown();
}

bool SyncController::OpenLog(const std::wstring& logDir)
{
	if (!logger_.Open(logDir))
	{
		return false;
	}

	SyncConfig config;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		config = config_;
	}

	logger_.Log(L"=== ProjectSync 启动 ===");
	logger_.Log(L"文件夹 A = " + config.RootA);
	logger_.Log(L"文件夹 B = " + config.RootB);
	return true;
}

void SyncController::Shutdown()
{
	cancel_.store(true);
	JoinWorker();
	logger_.Close();
}

void SyncController::SetConfig(const SyncConfig& config)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_ = config;

	// 未扫描前也把源/目标标签填好，界面上不至于显示空白
	const DirectionTargets targets = ResolveDirection(config_, snapshot_.Dir);
	snapshot_.SrcLabel = targets.SrcLabel;
	snapshot_.DstLabel = targets.DstLabel;
	snapshot_.SrcRoot = targets.Src;
	snapshot_.DstRoot = targets.Dst;

	// 配置变了，之前的差异结果作废
	snapshot_.Diff.Clear();
	snapshot_.HasVerify = false;
	snapshot_.VerifyPass = false;
	snapshot_.Message.clear();
	if (!snapshot_.Busy())
	{
		snapshot_.Stage = SyncStage::Idle;
	}
	++revision_;
}

SyncConfig SyncController::GetConfig() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return config_;
}

void SyncController::StartScan(SyncDirection direction, bool dryRun)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (snapshot_.Busy())
		{
			return;
		}
		// 先置位再起线程：外部轮询者立刻就能看到「忙」，不会误判为 Idle
		snapshot_.Stage = SyncStage::Scanning;
		++revision_;
	}

	JoinWorker();
	cancel_.store(false);
	worker_ = std::thread(&SyncController::RunScan, this, direction, dryRun);
}

void SyncController::StartExecute(bool ignorePreflight)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (snapshot_.Busy())
		{
			return;
		}
		snapshot_.Stage = SyncStage::Executing;
		++revision_;
	}

	JoinWorker();
	cancel_.store(false);
	worker_ = std::thread(&SyncController::RunExecute, this, ignorePreflight);
}

void SyncController::StartVerify()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (snapshot_.Busy())
		{
			return;
		}
		snapshot_.Stage = SyncStage::Verifying;
		++revision_;
	}

	JoinWorker();
	cancel_.store(false);
	worker_ = std::thread(&SyncController::RunVerify, this);
}

void SyncController::SetSelection(const std::vector<std::wstring>& selectedKeys)
{
	std::unordered_set<std::wstring> keys;
	keys.reserve(selectedKeys.size());
	for (const std::wstring& key : selectedKeys)
	{
		keys.insert(ToLowerInvariant(key));
	}

	std::lock_guard<std::mutex> lock(mutex_);
	for (DiffItem& item : snapshot_.Diff.ToCopy)
	{
		item.Selected = keys.count(ToLowerInvariant(item.Rel)) != 0;
	}
	for (DiffItem& item : snapshot_.Diff.ToDelete)
	{
		item.Selected = keys.count(ToLowerInvariant(item.Rel)) != 0;
	}
	++revision_;
}

void SyncController::Cancel()
{
	cancel_.store(true);
}

uint64_t SyncController::GetRevision() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return revision_;
}

SyncSnapshot SyncController::GetSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	SyncSnapshot out = snapshot_;
	out.LogPath = logger_.FilePath();
	out.WarnCount = logger_.WarnCount();
	out.ErrorCount = logger_.ErrorCount();
	out.LogTail = logger_.Tail(600);
	return out;
}

void SyncController::JoinWorker()
{
	if (worker_.joinable())
	{
		worker_.join();
	}
}

void SyncController::OnSinkProgress(const std::wstring& stage, uint64_t done, uint64_t total, const std::wstring& current)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.StageText = stage;
	snapshot_.Done = done;
	snapshot_.Total = total;
	snapshot_.CurrentItem = current;
	snapshot_.ProgressKnown = (total > 0);
	snapshot_.Progress = (total > 0)
		? static_cast<float>(static_cast<double>(done) / static_cast<double>(total))
		: 0.0f;
	++revision_;
}

void SyncController::SetStage(SyncStage stage)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.Stage = stage;
	++revision_;
}

void SyncController::SetStageText(const std::wstring& text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.StageText = text;
	++revision_;
}

void SyncController::SetMessage(const std::wstring& text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.Message = text;
	++revision_;
}

void SyncController::ResetProgress(const std::wstring& stageText)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.StageText = stageText;
	snapshot_.CurrentItem.clear();
	snapshot_.Done = 0;
	snapshot_.Total = 0;
	snapshot_.Progress = 0.0f;
	snapshot_.ProgressKnown = false;
	++revision_;
}

void SyncController::SetPreflightIssues(const std::vector<std::wstring>& issues)
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.PreflightIssues = issues;
	++revision_;
}

bool SyncController::ScanAndCompare(const SyncConfig& config, const DirectionTargets& targets, DiffResult& outDiff, IProgressSink* sink)
{
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::path(targets.Src), ec) || ec)
	{
		return false;
	}
	ec.clear();
	if (!std::filesystem::exists(std::filesystem::path(targets.Dst), ec) || ec)
	{
		return false;
	}

	FileScanner scanner(config);

	SetStageText(L"扫描 " + targets.SrcLabel + L" ...");
	const FileMap srcMap = scanner.Scan(targets.Src, sink);
	if (cancel_.load())
	{
		return false;
	}

	SetStageText(L"扫描 " + targets.DstLabel + L" ...");
	const FileMap dstMap = scanner.Scan(targets.Dst, sink);
	if (cancel_.load())
	{
		return false;
	}

	SetStageText(L"比对文件内容（大小 + SHA256）...");
	DiffEngine engine;
	outDiff = engine.Compare(srcMap, dstMap, targets.Src, targets.Dst, sink);

	logger_.Log(FormatW(L"diff: same=%llu copy=%llu delete=%llu",
	                    static_cast<unsigned long long>(outDiff.Same),
	                    static_cast<unsigned long long>(outDiff.ToCopy.size()),
	                    static_cast<unsigned long long>(outDiff.ToDelete.size())));
	return true;
}

void SyncController::RunScan(SyncDirection direction, bool dryRun)
{
	const SyncConfig config = GetConfig();
	const DirectionTargets targets = ResolveDirection(config, direction);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Dir = direction;
		snapshot_.DryRun = dryRun;
		snapshot_.SrcLabel = targets.SrcLabel;
		snapshot_.DstLabel = targets.DstLabel;
		snapshot_.SrcRoot = targets.Src;
		snapshot_.DstRoot = targets.Dst;
		snapshot_.Diff.Clear();
		snapshot_.Plan.Clear();
		snapshot_.Copy = PhaseReport();
		snapshot_.Backup = PhaseReport();
		snapshot_.Delete = PhaseReport();
		snapshot_.HasVerify = false;
		snapshot_.VerifyPass = false;
		snapshot_.PreflightIssues.clear();
		snapshot_.Message.clear();
		++revision_;
	}

	SetStage(SyncStage::Scanning);
	ResetProgress(L"准备扫描...");
	logger_.Log(FormatW(L"=== 扫描 %s -> %s%s ===", targets.SrcLabel.c_str(), targets.DstLabel.c_str(),
	                    dryRun ? L"（预演）" : L""));

	Sink sink(*this);
	DiffResult diff;
	if (!ScanAndCompare(config, targets, diff, &sink))
	{
		if (cancel_.load())
		{
			SetStage(SyncStage::Cancelled);
			SetMessage(L"已取消扫描。");
			logger_.Warn(L"扫描被取消");
			return;
		}

		const std::wstring text = L"无法访问目录：" + targets.Src + L" 或 " + targets.Dst;
		SetStage(SyncStage::Failed);
		SetMessage(text);
		logger_.Error(text);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Diff = std::move(diff);
		snapshot_.Done = snapshot_.Diff.ToCopy.size() + snapshot_.Diff.ToDelete.size();
		snapshot_.Total = snapshot_.Done;
		snapshot_.Progress = 1.0f;
		snapshot_.ProgressKnown = true;
		snapshot_.Message = snapshot_.Diff.Empty()
			? L"两边已完全一致，无需同步。"
			: L"扫描完成，请确认差异后执行同步。";
		++revision_;
	}

	SetStage(SyncStage::Ready);
	logger_.Log(L"扫描完成，等待用户确认");
}

void SyncController::RunExecute(bool ignorePreflight)
{
	DiffResult all;
	SyncDirection direction = SyncDirection::AToB;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		all = snapshot_.Diff;
		direction = snapshot_.Dir;
	}

	// 只同步勾选项：未勾选的差异保持原样
	const DiffResult plan = SelectOnly(all);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Plan = plan;
		++revision_;
	}

	const SyncConfig config = GetConfig();
	const DirectionTargets targets = ResolveDirection(config, direction);

	if (plan.Empty())
	{
		SetStage(SyncStage::Ready);
		SetMessage(all.Empty() ? L"没有需要同步的内容。" : L"没有勾选任何差异项。");
		return;
	}

	const std::vector<std::wstring> issues = Preflight::Check(config, true);
	if (!issues.empty() && !ignorePreflight)
	{
		SetPreflightIssues(issues);
		SetStage(SyncStage::Blocked);
		SetMessage(L"运行前检查未通过：请处理后重试，或选择忽略并继续。");
		logger_.Warn(L"运行前检查未通过：" + JoinIssues(issues));
		return;
	}

	const std::wstring stamp = TimeStamp();
	SetStage(SyncStage::Executing);
	ResetProgress(L"复制（覆盖 + 新增）...");
	logger_.Log(FormatW(L"=== 执行 %s -> %s，stamp=%s ===", targets.SrcLabel.c_str(), targets.DstLabel.c_str(), stamp.c_str()));

	Sink sink(*this);
	SyncExecutor executor(config, logger_);

	// 阶段 1/3：复制
	const PhaseReport copyReport = executor.CopyPhase(plan, targets.Src, targets.Dst, &sink);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Copy = copyReport;
		++revision_;
	}
	logger_.Ok(FormatW(L"复制 %llu / %llu 个文件",
	                   static_cast<unsigned long long>(copyReport.Ok),
	                   static_cast<unsigned long long>(copyReport.Total)));

	// 阶段 2/3：备份
	PhaseReport backupReport;
	if (!cancel_.load())
	{
		ResetProgress(L"备份待删文件...");
		backupReport = executor.BackupPhase(plan, targets.Dst, stamp, &sink);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			snapshot_.Backup = backupReport;
			++revision_;
		}
		logger_.Ok(FormatW(L"备份 %llu / %llu 个文件 -> %s",
		                   static_cast<unsigned long long>(backupReport.Ok),
		                   static_cast<unsigned long long>(backupReport.Total),
		                   backupReport.BackupRoot.c_str()));
	}

	// 阶段 3/3 之一：删除
	PhaseReport deleteReport;
	if (!cancel_.load())
	{
		ResetProgress(L"删除目标侧多余文件...");
		deleteReport = executor.DeletePhase(plan, targets.Dst, &sink);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			snapshot_.Delete = deleteReport;
			++revision_;
		}
		logger_.Ok(FormatW(L"删除 %llu / %llu 个文件，回收空目录 %llu 个",
		                   static_cast<unsigned long long>(deleteReport.Ok),
		                   static_cast<unsigned long long>(deleteReport.Total),
		                   static_cast<unsigned long long>(deleteReport.RemovedDirs)));
	}

	// 阶段 3/3 之二：复核。即使中途取消也照做：只读操作，能让用户看到当前真实状态
	SetStage(SyncStage::Verifying);
	ResetProgress(L"复核（重新扫描比对）...");
	DiffResult verifyDiff;
	const bool scanned = ScanAndCompare(config, targets, verifyDiff, &sink);

	// 部分同步的复核语义：只判断「本次勾选的项」是否都已一致。
	// 未勾选的差异仍留在列表里，不算失败。
	bool verifyPass = scanned;
	if (scanned)
	{
		std::unordered_set<std::wstring> remaining;
		remaining.reserve(verifyDiff.ToCopy.size() + verifyDiff.ToDelete.size());
		for (const DiffItem& item : verifyDiff.ToCopy)
		{
			remaining.insert(ToLowerInvariant(item.Rel));
		}
		for (const DiffItem& item : verifyDiff.ToDelete)
		{
			remaining.insert(ToLowerInvariant(item.Rel));
		}

		for (const DiffItem& item : plan.ToCopy)
		{
			if (remaining.count(ToLowerInvariant(item.Rel)) != 0)
			{
				verifyPass = false;
				break;
			}
		}
		if (verifyPass)
		{
			for (const DiffItem& item : plan.ToDelete)
			{
				if (remaining.count(ToLowerInvariant(item.Rel)) != 0)
				{
					verifyPass = false;
					break;
				}
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Diff = verifyDiff;
		snapshot_.HasVerify = scanned;
		snapshot_.VerifyPass = verifyPass;
		++revision_;
	}

	const uint64_t failedCount = static_cast<uint64_t>(copyReport.Errors.size()
	                                                   + backupReport.Errors.size()
	                                                   + deleteReport.Errors.size());
	const uint64_t plannedCount = static_cast<uint64_t>(plan.ToCopy.size() + plan.ToDelete.size());
	const uint64_t remainingCount = static_cast<uint64_t>(verifyDiff.ToCopy.size() + verifyDiff.ToDelete.size());

	if (cancel_.load())
	{
		SetStage(SyncStage::Cancelled);
		SetMessage(L"已取消：目标侧可能处于中间状态，请重新执行一次同步使其收敛。");
		logger_.Warn(L"执行被取消");
		return;
	}

	if (!scanned)
	{
		SetStage(SyncStage::Failed);
		SetMessage(L"复核失败：无法重新扫描目录，请查看日志。");
		logger_.Error(L"复核失败：无法重新扫描目录");
		return;
	}

	if (verifyPass)
	{
		std::wstring text = FormatW(L"本次勾选的 %llu 项已同步一致",
		                            static_cast<unsigned long long>(plannedCount));
		if (remainingCount > 0)
		{
			text += FormatW(L"；列表仍有 %llu 项差异（本次未勾选）",
			                static_cast<unsigned long long>(remainingCount));
		}

		SetStage(SyncStage::Done);
		SetMessage(failedCount == 0 ? text : text + FormatW(L"，但有 %llu 项操作失败，见日志。",
		                                                    static_cast<unsigned long long>(failedCount)));
		logger_.Ok(text);
		return;
	}

	const std::wstring text = FormatW(L"复核未通过：本次勾选的 %llu 项中仍有未完成项（列表剩余 %llu 项）。常见原因：文件被占用或权限不足。",
	                                  static_cast<unsigned long long>(plannedCount),
	                                  static_cast<unsigned long long>(remainingCount));
	SetStage(SyncStage::Failed);
	SetMessage(text);
	logger_.Error(text);
}

void SyncController::RunVerify()
{
	const SyncConfig config = GetConfig();
	SyncDirection direction = SyncDirection::AToB;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		direction = snapshot_.Dir;
	}

	const DirectionTargets targets = ResolveDirection(config, direction);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.SrcLabel = targets.SrcLabel;
		snapshot_.DstLabel = targets.DstLabel;
		snapshot_.SrcRoot = targets.Src;
		snapshot_.DstRoot = targets.Dst;
		snapshot_.PreflightIssues.clear();
		++revision_;
	}

	SetStage(SyncStage::Verifying);
	ResetProgress(L"一致性校验...");
	logger_.Log(L"=== 一致性校验 ===");

	Sink sink(*this);
	DiffResult diff;
	const bool scanned = ScanAndCompare(config, targets, diff, &sink);
	const bool pass = scanned && diff.Empty();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_.Diff = diff;
		snapshot_.HasVerify = scanned;
		snapshot_.VerifyPass = pass;
		++revision_;
	}

	if (!scanned)
	{
		SetStage(SyncStage::Failed);
		SetMessage(cancel_.load() ? L"已取消。" : L"校验失败：无法扫描目录，请查看日志。");
		return;
	}

	if (pass)
	{
		const std::wstring text = FormatW(L"两边完全一致（%llu 个同步文件）",
		                                  static_cast<unsigned long long>(diff.Same));
		SetStage(SyncStage::Done);
		SetMessage(text);
		logger_.Ok(text);
		return;
	}

	const std::wstring text = FormatW(L"存在差异：%llu 项待覆盖、%llu 项待删除，%llu 项内容相同。",
	                                  static_cast<unsigned long long>(diff.ToCopy.size()),
	                                  static_cast<unsigned long long>(diff.ToDelete.size()),
	                                  static_cast<unsigned long long>(diff.Same));
	SetStage(SyncStage::Ready);
	SetMessage(text);
	logger_.Log(text);
}
