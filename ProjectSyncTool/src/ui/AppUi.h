#pragma once

#include "core/SyncConfig.h"
#include "core/SyncController.h"

#include <cstdint>
#include <string>
#include <vector>

// 界面层。只做三件事：读控制器快照、绘制、派发命令。
// 不直接访问文件系统（除文件夹/文件选择对话框），也不持有业务数据。
class AppUi
{
public:
	bool Init(void* hwnd, float dpiScale);	// 载入配置、打开日志、按 DPI 加载中文字体
	void Shutdown();
	void Draw();							// 每帧调用（需已在 ImGui 帧内）

	// 资源管理器拖入的文件夹（客户端坐标）；由窗口消息处理函数调用
	void QueueFolderDrop(int clientX, int clientY, const std::wstring& path);

private:
	// 差异表的一行；Item 指向当前快照里的差异项，可直接改勾选状态
	struct DisplayRow
	{
		bool IsDelete = false;
		DiffItem* Item = nullptr;
	};

	// 一个输入框在客户区里的矩形（用于判断拖拽落在哪个框上）
	struct FieldRect
	{
		float X0 = 0.0f;
		float Y0 = 0.0f;
		float X1 = 0.0f;
		float Y1 = 0.0f;
		bool Valid = false;
	};

	struct FolderDrop
	{
		int X = 0;
		int Y = 0;
		std::wstring Path;
	};

	void RefreshSnapshot();
	void ProcessPendingDrops();
	void RecordFieldRect(FieldRect& rect) const;
	void RebuildRows();
	void FlushSelection();					// 把勾选变化交回控制器
	void ApplySelectionPolicy(int policy);	// 0 全选 1 全不选 2 反选 3 仅新增覆盖

	// 差异行右键菜单：在 A / B 侧的资源管理器中定位该文件
	void DrawDiffRowMenu(const DisplayRow& row);
	std::wstring RevealInSide(const std::wstring& root, const std::wstring& sideName, const std::wstring& rel);
	void RevealInBothSides(const SyncConfig& config, const std::wstring& rel);

	void DrawRoot();
	void DrawSyncTab();
	void DrawConfigTab();
	void DrawLogTab();
	void DrawConfirmModal();
	void DrawPreflightModal();
	void DrawExecutionResult();

	void RequestCompare();
	void RequestExecute();

	void LoadEditorsFromConfig();
	void BuildConfigFromEditors(SyncConfig& config) const;
	bool SaveConfigTo(const std::wstring& path, const std::wstring& message);
	bool LoadConfigFrom(const std::wstring& path, const std::wstring& message);

	bool PickFolder(std::wstring& out);
	bool PickConfigFile(bool save, std::wstring& out);

	SyncController controller_;
	SyncSnapshot snap_;
	uint64_t snapshotRevision_ = 0;

	void* hwnd_ = nullptr;
	std::wstring exeDir_;
	std::wstring configPath_;
	std::wstring settingsPath_;
	std::wstring logDir_;
	std::wstring notice_;
	std::wstring revealNotice_;		// 右键「在 A / B 中打开」的结果提示（同步页显示）

	// 配置页编辑缓冲（UTF-8）
	std::string editA_;
	std::string editB_;
	std::string editBackupDir_;
	std::string editAnyName_;
	std::string editRootName_;
	std::string editPathPrefix_;
	std::string editRootExt_;
	bool reloadEditors_ = true;

	std::vector<DisplayRow> rows_;
	std::string filterBuffer_;
	bool rowsShowNew_ = true;
	bool rowsShowModified_ = true;
	bool rowsShowDelete_ = true;
	bool selectionDirty_ = false;

	SyncDirection direction_ = SyncDirection::AToB;
	bool requestConfirm_ = false;
	bool requestPreflight_ = false;
	bool fontLoaded_ = false;
	bool autoScrollLog_ = true;

	std::vector<FolderDrop> pendingDrops_;
	FieldRect aFieldRect_;
	FieldRect bFieldRect_;
	FieldRect backupFieldRect_;
	int forceTab_ = -1;		// 0 同步 1 配置 2 日志：本帧强制切到该页
};
