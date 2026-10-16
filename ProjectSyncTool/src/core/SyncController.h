#pragma once

#include "core/Logger.h"
#include "core/Progress.h"
#include "core/SyncConfig.h"
#include "core/SyncTypes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 控制器状态机
enum class SyncStage
{
	Idle,		// 尚未扫描
	Scanning,	// 扫描 + 比对中
	Ready,		// 已有差异结果，等待用户决定是否执行
	Blocked,	// 运行前检查未通过，等待用户确认
	Executing,	// 复制 / 备份 / 删除中
	Verifying,	// 复核中
	Done,		// 执行并复核通过（或校验一致）
	Cancelled,	// 用户取消
	Failed		// 出错
};

// UI 每帧读取的状态快照（值语义，UI 不直接碰 worker 数据）
struct SyncSnapshot
{
	SyncStage Stage = SyncStage::Idle;
	SyncDirection Dir = SyncDirection::AToB;
	bool DryRun = true;
	std::wstring SrcLabel;
	std::wstring DstLabel;
	std::wstring SrcRoot;
	std::wstring DstRoot;

	// 当前差异：Ready / Blocked 时是待执行计划，执行后是复核剩余差异。
	// DiffItem::Selected 表示界面勾选状态，只有勾选项会被同步。
	DiffResult Diff;
	// 本次实际执行的计划（执行前勾选项的快照）
	DiffResult Plan;
	PhaseReport Copy;
	PhaseReport Backup;
	PhaseReport Delete;

	bool HasVerify = false;
	bool VerifyPass = false;

	std::wstring StageText;		// 阶段描述
	std::wstring CurrentItem;	// 当前处理项
	uint64_t Done = 0;
	uint64_t Total = 0;
	float Progress = 0.0f;
	bool ProgressKnown = false;

	std::wstring Message;		// 结果 / 提示

	std::vector<std::wstring> PreflightIssues;

	std::wstring LogPath;
	uint64_t WarnCount = 0;
	uint64_t ErrorCount = 0;
	std::vector<std::wstring> LogTail;

	bool Busy() const
	{
		return Stage == SyncStage::Scanning || Stage == SyncStage::Executing || Stage == SyncStage::Verifying;
	}
};

// 唯一的业务入口：持有配置、日志与 worker 线程，向 UI 暴露快照与命令。
// 所有耗时操作都在 worker 线程执行，UI 线程只读快照。
class SyncController
{
public:
	SyncController() = default;
	~SyncController();

	// 打开 logs\sync-<时间戳>.log
	bool OpenLog(const std::wstring& logDir);

	// 取消并等待 worker 退出（UI 关闭时调用）
	void Shutdown();

	void SetConfig(const SyncConfig& config);
	SyncConfig GetConfig() const;

	void StartScan(SyncDirection direction, bool dryRun);
	void StartExecute(bool ignorePreflight);
	void StartVerify();
	void Cancel();

	// 界面勾选结果回传（键 = 相对路径）；只有勾选项会被同步
	void SetSelection(const std::vector<std::wstring>& selectedKeys);

	uint64_t GetRevision() const;
	SyncSnapshot GetSnapshot() const;

private:
	class Sink;

	void JoinWorker();
	void OnSinkProgress(const std::wstring& stage, uint64_t done, uint64_t total, const std::wstring& current);

	void SetStage(SyncStage stage);
	void SetStageText(const std::wstring& text);
	void SetMessage(const std::wstring& text);
	void ResetProgress(const std::wstring& stageText);
	void SetPreflightIssues(const std::vector<std::wstring>& issues);

	void RunScan(SyncDirection direction, bool dryRun);
	void RunExecute(bool ignorePreflight);
	void RunVerify();

	// 扫描两侧并比对；失败（目录不可访问）返回 false
	bool ScanAndCompare(const SyncConfig& config, const DirectionTargets& targets, DiffResult& outDiff, IProgressSink* sink);

	SyncConfig config_;
	Logger logger_;

	mutable std::mutex mutex_;
	SyncSnapshot snapshot_;
	uint64_t revision_ = 0;

	std::thread worker_;
	std::atomic_bool cancel_{false};
};
