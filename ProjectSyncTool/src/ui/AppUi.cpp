#include "ui/AppUi.h"

#include "core/DiffEngine.h"
#include "core/StringUtil.h"

#include "imgui.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>

#include <cfloat>
#include <cwchar>
#include <filesystem>
#include <sstream>

namespace
{
const char* const kConfirmPopup = "确认执行同步";
const char* const kPreflightPopup = "运行前检查未通过";

// ---- ImGui 与 std::string 的桥接：让输入框能自动扩容 ----
int InputTextResizeCallback(ImGuiInputTextCallbackData* data)
{
	if (data->EventFlag != ImGuiInputTextFlags_CallbackResize)
	{
		return 0;
	}

	std::string* text = static_cast<std::string*>(data->UserData);
	IM_ASSERT(data->Buf == text->c_str());
	text->resize(static_cast<size_t>(data->BufTextLen));
	data->Buf = text->data();
	return 0;
}

bool InputTextStd(const char* label, std::string& value)
{
	const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
	return ImGui::InputText(label, value.data(), value.capacity() + 1, flags, InputTextResizeCallback, &value);
}

bool InputTextMultilineStd(const char* label, std::string& value, const ImVec2& size)
{
	const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
	return ImGui::InputTextMultiline(label, value.data(), value.capacity() + 1, size, flags, InputTextResizeCallback, &value);
}

// 一行一项的文本 <-> 列表
std::vector<std::wstring> ParseLines(const std::string& text)
{
	std::vector<std::wstring> items;
	std::istringstream stream(text);
	std::string raw;
	while (std::getline(stream, raw))
	{
		while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t'))
		{
			raw.pop_back();
		}

		size_t begin = 0;
		while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t'))
		{
			++begin;
		}

		if (begin >= raw.size())
		{
			continue;
		}
		items.push_back(FromUtf8(raw.substr(begin)));
	}
	return items;
}

std::string JoinLines(const std::vector<std::wstring>& items)
{
	std::string text;
	for (const std::wstring& item : items)
	{
		text += ToUtf8(item);
		text += "\n";
	}
	return text;
}

void OpenWithShell(const std::wstring& target)
{
	if (target.empty())
	{
		return;
	}
	::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 在资源管理器中定位 root\rel：文件存在则选中它，否则打开最近的已存在目录。
// 返回实际打开的路径；返回空表示该侧根目录不可用。
std::wstring RevealInExplorer(const std::wstring& root, const std::wstring& rel)
{
	if (root.empty() || rel.empty())
	{
		return std::wstring();
	}

	const std::wstring full = JoinPath(root, rel);
	std::error_code ec;
	if (std::filesystem::is_regular_file(std::filesystem::path(full), ec))
	{
		// /select 的路径带空格时必须加引号，整串作为 explorer 的参数
		const std::wstring args = L"/select,\"" + full + L"\"";
		::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
		return full;
	}

	// 该侧没有这个文件（新增项的目标侧 / 删除项的源侧）：向上退到最近的已存在目录，不越过 root
	std::filesystem::path dir = std::filesystem::path(full).parent_path();
	const std::filesystem::path rootPath = std::filesystem::path(TrimTrailingSlash(root));
	while (dir != rootPath && dir.has_parent_path() && !std::filesystem::is_directory(dir, ec))
	{
		dir = dir.parent_path();
	}
	if (dir.empty() || !std::filesystem::is_directory(dir, ec))
	{
		return std::wstring();
	}

	OpenWithShell(dir.wstring());
	return dir.wstring();
}

// 把非空说明追加到汇总文本，多条之间用「；」分隔
void AppendNotice(std::wstring& text, const std::wstring& note)
{
	if (note.empty())
	{
		return;
	}
	if (!text.empty())
	{
		text += L"；";
	}
	text += note;
}

std::string ToUtf8String(const std::wstring& text)
{
	return ToUtf8(text);
}

const char* StageName(SyncStage stage)
{
	switch (stage)
	{
	case SyncStage::Idle:		return "空闲";
	case SyncStage::Scanning:	return "对比中";
	case SyncStage::Ready:		return "已就绪";
	case SyncStage::Blocked:	return "等待确认";
	case SyncStage::Executing:	return "同步中";
	case SyncStage::Verifying:	return "复核中";
	case SyncStage::Done:		return "已完成";
	case SyncStage::Cancelled:	return "已取消";
	case SyncStage::Failed:		return "失败";
	}
	return "未知";
}

ImVec4 StageColor(SyncStage stage)
{
	switch (stage)
	{
	case SyncStage::Done:		return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
	case SyncStage::Failed:		return ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
	case SyncStage::Cancelled:	return ImVec4(1.00f, 0.75f, 0.35f, 1.0f);
	case SyncStage::Blocked:	return ImVec4(1.00f, 0.75f, 0.35f, 1.0f);
	case SyncStage::Ready:		return ImVec4(0.60f, 0.80f, 1.00f, 1.0f);
	default:					return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
	}
}
}	// namespace

bool AppUi::Init(void* hwnd, float dpiScale)
{
	hwnd_ = hwnd;
	exeDir_ = GetExeDirectory();
	settingsPath_ = DefaultSettingsPath();
	logDir_ = JoinPath(exeDir_, L"logs");

	// 上次用过的配置文件优先，否则用 exe 旁的默认配置
	std::wstring lastConfig;
	if (!LoadLastConfigPath(settingsPath_, lastConfig) || lastConfig.empty())
	{
		lastConfig = DefaultConfigPath();
	}
	configPath_ = lastConfig;

	SyncConfig config = SyncConfig::Defaults();
	config.Load(configPath_);
	controller_.SetConfig(config);
	controller_.OpenLog(logDir_);

	if (dpiScale < 1.0f)
	{
		dpiScale = 1.0f;
	}
	const float fontSize = 18.0f * dpiScale;

	// 中文字体：默认字体没有 CJK 字形，不加载则中文全是方块。
	// 1.92+ 的 DX11 后端支持动态字体纹理，字形按需栅格化，无需指定 GlyphRanges。
	const char* const candidates[] = {
		"C:\\Windows\\Fonts\\msyh.ttc",
		"C:\\Windows\\Fonts\\msyhl.ttc",
		"C:\\Windows\\Fonts\\simhei.ttf",
		"C:\\Windows\\Fonts\\simsun.ttc",
	};

	ImGuiIO& io = ImGui::GetIO();
	fontLoaded_ = false;
	for (const char* path : candidates)
	{
		std::error_code ec;
		if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec)
		{
			continue;
		}
		if (io.Fonts->AddFontFromFileTTF(path, fontSize) != nullptr)
		{
			fontLoaded_ = true;
			break;
		}
	}
	if (!fontLoaded_)
	{
		io.Fonts->AddFontDefault();
	}

	notice_ = fontLoaded_ ? L"配置与日志已就绪。" : L"未能加载中文字体，界面与文件名可能显示为方块。";
	return true;
}

void AppUi::Shutdown()
{
	controller_.Shutdown();
}

void AppUi::RefreshSnapshot()
{
	const uint64_t revision = controller_.GetRevision();
	if (revision == snapshotRevision_)
	{
		return;
	}

	snapshotRevision_ = revision;
	snap_ = controller_.GetSnapshot();

	if (snap_.Stage == SyncStage::Blocked && !snap_.PreflightIssues.empty())
	{
		requestPreflight_ = true;
	}
}

void AppUi::Draw()
{
	RefreshSnapshot();

	// 拖拽命中判断用上一帧记录的输入框矩形；随后清空，本帧绘制配置页时重录
	ProcessPendingDrops();
	aFieldRect_ = FieldRect();
	bFieldRect_ = FieldRect();

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::Begin("##ProjectSyncRoot", nullptr, flags);
	DrawRoot();
	ImGui::End();
}

void AppUi::DrawRoot()
{
	ImGui::TextUnformatted("ProjectSync");

	ImGui::Separator();

	if (ImGui::BeginTabBar("##tabs"))
	{
		if (ImGui::BeginTabItem("同步", nullptr, forceTab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0))
		{
			DrawSyncTab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("配置", nullptr, forceTab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0))
		{
			DrawConfigTab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("日志", nullptr, forceTab_ == 2 ? ImGuiTabItemFlags_SetSelected : 0))
		{
			DrawLogTab();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	forceTab_ = -1;

	if (requestConfirm_)
	{
		ImGui::OpenPopup(kConfirmPopup);
		requestConfirm_ = false;
	}
	if (requestPreflight_)
	{
		ImGui::OpenPopup(kPreflightPopup);
		requestPreflight_ = false;
	}

	DrawConfirmModal();
	DrawPreflightModal();
}

void AppUi::DrawSyncTab()
{
	const SyncConfig config = controller_.GetConfig();
	const bool busy = snap_.Busy();
	const bool directionMatches = (snap_.Dir == direction_);
	const uint64_t selectedCount = CountSelected(snap_.Diff);
	const bool hasDiff = !snap_.Diff.Empty();
	const bool canExecute = !busy && hasDiff && directionMatches && selectedCount > 0;
	// 方向已切换：下方列表仍是上一次对比的结果，整块灰显表示不可用（切回原方向即可恢复）
	const bool listDisabled = hasDiff && !directionMatches;

	// ---- 方向：直接用两侧文件夹名 ----
	const std::wstring nameA = LeafName(config.RootA, L"文件夹 A");
	const std::wstring nameB = LeafName(config.RootB, L"文件夹 B");
	const std::string forwardLabel = ToUtf8String(nameA) + " -> " + ToUtf8String(nameB) + "##forward";
	const std::string backwardLabel = ToUtf8String(nameB) + " -> " + ToUtf8String(nameA) + "##backward";

	ImGui::BeginDisabled(busy);
	const bool leftToRight = (direction_ == SyncDirection::AToB);
	if (ImGui::RadioButton(forwardLabel.c_str(), leftToRight))
	{
		direction_ = SyncDirection::AToB;
	}
	ImGui::SameLine();
	if (ImGui::RadioButton(backwardLabel.c_str(), !leftToRight))
	{
		direction_ = SyncDirection::BToA;
	}
	ImGui::EndDisabled();

	ImGui::TextDisabled("%s : %s", ToUtf8String(nameA).c_str(), ToUtf8String(config.RootA).c_str());
	ImGui::TextDisabled("%s : %s", ToUtf8String(nameB).c_str(), ToUtf8String(config.RootB).c_str());

	// ---- 操作 ----
	ImGui::BeginDisabled(busy);
	if (ImGui::Button("差异对比", ImVec2(110.0f, 0.0f)))
	{
		RequestCompare();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(!canExecute);
	if (ImGui::Button("执行同步", ImVec2(110.0f, 0.0f)))
	{
		RequestExecute();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button("一致性校验", ImVec2(110.0f, 0.0f)))
	{
		controller_.StartVerify();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(!busy);
	if (ImGui::Button("取消", ImVec2(80.0f, 0.0f)))
	{
		controller_.Cancel();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::TextDisabled("状态：%s", StageName(snap_.Stage));

	if (!busy && !canExecute)
	{
		if (!hasDiff)
		{
			ImGui::TextDisabled("请先执行「差异对比」。");
		}
		else if (!directionMatches)
		{
			ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.35f, 1.0f), "同步方向已改变，请重新执行「差异对比」。");
		}
		else if (selectedCount == 0)
		{
			ImGui::TextDisabled("请在下方差异列表中勾选要同步的项。");
		}
	}

	// ---- 进度 ----
	if (busy)
	{
		const std::string stageText = ToUtf8String(snap_.StageText);
		if (snap_.ProgressKnown)
		{
			const std::string overlay = stageText + "  " + std::to_string(snap_.Done) + " / " + std::to_string(snap_.Total);
			ImGui::ProgressBar(snap_.Progress, ImVec2(-FLT_MIN, 0.0f), overlay.c_str());
		}
		else
		{
			ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0.0f), stageText.c_str());
		}
		ImGui::TextDisabled("%s", ToUtf8String(snap_.CurrentItem).c_str());
	}

	// ---- 摘要 ----
	ImGui::Separator();
	ImGui::Text("差异摘要（源：%s -> 目标：%s）",
	            ToUtf8String(snap_.SrcLabel).c_str(),
	            ToUtf8String(snap_.DstLabel).c_str());
	ImGui::Text("内容相同     : %llu", static_cast<unsigned long long>(snap_.Diff.Same));
	ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.35f, 1.0f), "需覆盖 / 新增 : %llu   (%s)",
	                   static_cast<unsigned long long>(snap_.Diff.ToCopy.size()),
	                   ToUtf8String(FormatBytes(snap_.Diff.CopyBytes)).c_str());
	ImGui::TextColored(ImVec4(1.00f, 0.50f, 0.45f, 1.0f), "需删除       : %llu   (%s)",
	                   static_cast<unsigned long long>(snap_.Diff.ToDelete.size()),
	                   ToUtf8String(FormatBytes(snap_.Diff.DeleteBytes)).c_str());
	ImGui::TextColored(ImVec4(0.60f, 0.85f, 1.00f, 1.0f), "已勾选       : %llu 项（将按勾选项同步）",
	                   static_cast<unsigned long long>(selectedCount));

	if (!snap_.Message.empty())
	{
		ImGui::TextColored(StageColor(snap_.Stage), "%s", ToUtf8String(snap_.Message).c_str());
	}

	DrawExecutionResult();

	if (!revealNotice_.empty())
	{
		ImGui::TextColored(ImVec4(0.60f, 0.85f, 1.00f, 1.0f), "%s", ToUtf8String(revealNotice_).c_str());
	}

	// ---- 过滤与选择（方向不匹配时整块灰显且不可操作）----
	if (listDisabled)
	{
		ImGui::BeginDisabled(true);
	}

	ImGui::Separator();
	ImGui::TextUnformatted("过滤");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(220.0f);
	InputTextStd("##filter", filterBuffer_);
	ImGui::SameLine();
	ImGui::Checkbox("新增", &rowsShowNew_);
	ImGui::SameLine();
	ImGui::Checkbox("覆盖", &rowsShowModified_);
	ImGui::SameLine();
	ImGui::Checkbox("删除", &rowsShowDelete_);

	RebuildRows();

	ImGui::TextUnformatted("勾选");
	ImGui::SameLine();
	if (ImGui::Button("全选"))
	{
		ApplySelectionPolicy(0);
	}
	ImGui::SameLine();
	if (ImGui::Button("全不选"))
	{
		ApplySelectionPolicy(1);
	}
	ImGui::SameLine();
	if (ImGui::Button("反选"))
	{
		ApplySelectionPolicy(2);
	}
	ImGui::SameLine();
	if (ImGui::Button("仅新增/覆盖"))
	{
		ApplySelectionPolicy(3);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("显示 %llu 项，已勾选 %llu 项（以上按钮作用于当前显示的项）",
	                    static_cast<unsigned long long>(rows_.size()),
	                    static_cast<unsigned long long>(selectedCount));

	// ---- 差异表 ----
	const float tableHeight = ImGui::GetContentRegionAvail().y - 4.0f;
	const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
		| ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("##diff", 5, tableFlags, ImVec2(0.0f, tableHeight > 120.0f ? tableHeight : 120.0f)))
	{
		if (listDisabled)
		{
			ImGui::EndDisabled();
		}
		FlushSelection();
		return;
	}

	ImGui::TableSetupColumn("选", ImGuiTableColumnFlags_WidthFixed, 34.0f);
	ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 60.0f);
	ImGui::TableSetupColumn("相对路径", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("去向", ImGuiTableColumnFlags_WidthFixed, 130.0f);
	ImGui::TableSetupScrollFreeze(0, 1);

	// 表头（手写：第 0 列放「全选当前显示项」复选框）
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	ImGui::TableSetColumnIndex(0);
	bool allShownSelected = !rows_.empty();
	for (const DisplayRow& row : rows_)
	{
		if (!row.Item->Selected)
		{
			allShownSelected = false;
			break;
		}
	}
	ImGui::PushID("shown-select-all");
	if (ImGui::Checkbox("##all", &allShownSelected))
	{
		for (DisplayRow& row : rows_)
		{
			row.Item->Selected = allShownSelected;
		}
		selectionDirty_ = true;
	}
	ImGui::PopID();
	ImGui::TableSetColumnIndex(1);
	ImGui::TableHeader("类型");
	ImGui::TableSetColumnIndex(2);
	ImGui::TableHeader("相对路径");
	ImGui::TableSetColumnIndex(3);
	ImGui::TableHeader("大小");
	ImGui::TableSetColumnIndex(4);
	ImGui::TableHeader("去向");

	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows_.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			DisplayRow& row = rows_[static_cast<size_t>(i)];
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(i);
			bool selected = row.Item->Selected;
			if (ImGui::Checkbox("##sel", &selected))
			{
				row.Item->Selected = selected;
				selectionDirty_ = true;
			}
			ImGui::PopID();

			// 本行内容条的纵向范围：右键命中判断用（各列等高，取第 0 项即可）
			const ImVec2 rowRectMin = ImGui::GetItemRectMin();
			const ImVec2 rowRectMax = ImGui::GetItemRectMax();

			ImGui::TableSetColumnIndex(1);
			if (row.IsDelete)
			{
				ImGui::TextColored(ImVec4(1.00f, 0.50f, 0.45f, 1.0f), "删除");
			}
			else if (row.Item->Kind == ChangeKind::New)
			{
				ImGui::TextColored(ImVec4(0.50f, 0.90f, 0.60f, 1.0f), "新增");
			}
			else
			{
				ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.35f, 1.0f), "覆盖");
			}

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(ToUtf8String(row.Item->Rel).c_str());

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(ToUtf8String(FormatBytes(row.Item->Size)).c_str());

			ImGui::TableSetColumnIndex(4);
			if (row.IsDelete)
			{
				ImGui::TextDisabled("%s（删除）", ToUtf8String(snap_.DstLabel).c_str());
			}
			else
			{
				ImGui::TextDisabled("-> %s", ToUtf8String(snap_.DstLabel).c_str());
			}

			// ---- 右键任意一列：在 A / B 中定位该文件（列表灰显时随 disabled 一并不可用）----
			ImGui::PushID(i);
			const float rowPadY = ImGui::GetStyle().CellPadding.y;
			const float mouseY = ImGui::GetMousePos().y;
			if (ImGui::TableGetHoveredColumn() >= 0
			    && mouseY >= rowRectMin.y - rowPadY && mouseY <= rowRectMax.y + rowPadY
			    && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				ImGui::OpenPopup("##rowmenu");
			}
			DrawDiffRowMenu(row);
			ImGui::PopID();
		}
	}

	ImGui::EndTable();

	if (listDisabled)
	{
		ImGui::EndDisabled();
	}

	FlushSelection();
}

// 差异行右键菜单。ID 由调用方 PushID(行号) 区分，内部只认 "##rowmenu"
void AppUi::DrawDiffRowMenu(const DisplayRow& row)
{
	if (!ImGui::BeginPopup("##rowmenu"))
	{
		return;
	}

	// 用扫描时的配置：A / B 是物理目录，与当前同步方向无关
	const SyncConfig config = controller_.GetConfig();
	const std::wstring nameA = LeafName(config.RootA, L"文件夹 A");
	const std::wstring nameB = LeafName(config.RootB, L"文件夹 B");

	ImGui::TextDisabled("%s", ToUtf8String(row.Item->Rel).c_str());
	ImGui::Separator();

	const std::string openA = "在 " + ToUtf8String(nameA) + " 中打开";
	const std::string openB = "在 " + ToUtf8String(nameB) + " 中打开";
	if (ImGui::MenuItem(openA.c_str(), nullptr, false, !config.RootA.empty()))
	{
		revealNotice_ = RevealInSide(config.RootA, nameA, row.Item->Rel);
	}
	if (ImGui::MenuItem(openB.c_str(), nullptr, false, !config.RootB.empty()))
	{
		revealNotice_ = RevealInSide(config.RootB, nameB, row.Item->Rel);
	}

	ImGui::Separator();
	if (ImGui::MenuItem("同时打开两个文件夹", nullptr, false, !config.RootA.empty() && !config.RootB.empty()))
	{
		RevealInBothSides(config, row.Item->Rel);
	}

	ImGui::EndPopup();
}

// 在某一侧定位文件；返回给用户的说明（空 = 该侧已精确选中文件，无需提示）
std::wstring AppUi::RevealInSide(const std::wstring& root, const std::wstring& sideName, const std::wstring& rel)
{
	const std::wstring opened = RevealInExplorer(root, rel);
	if (opened.empty())
	{
		return L"无法打开该侧目录：" + root;
	}

	// 该侧没有这个文件时打开的是上层目录，明确提示，避免看起来像定位错了
	const std::wstring full = JoinPath(root, rel);
	if (IEquals(opened, full))
	{
		return std::wstring();
	}
	return sideName + L" 侧没有 " + rel + L"，已打开 " + opened;
}

void AppUi::RevealInBothSides(const SyncConfig& config, const std::wstring& rel)
{
	revealNotice_.clear();
	AppendNotice(revealNotice_, RevealInSide(config.RootA, LeafName(config.RootA, L"文件夹 A"), rel));
	AppendNotice(revealNotice_, RevealInSide(config.RootB, LeafName(config.RootB, L"文件夹 B"), rel));
}

void AppUi::DrawExecutionResult()
{
	const bool hasResult = snap_.HasVerify || !snap_.Plan.Empty();
	if (!hasResult)
	{
		return;
	}

	ImGui::Separator();
	ImGui::Text("上次执行：计划 %llu 项（复制 %llu / 删除 %llu）",
	            static_cast<unsigned long long>(snap_.Plan.ToCopy.size() + snap_.Plan.ToDelete.size()),
	            static_cast<unsigned long long>(snap_.Plan.ToCopy.size()),
	            static_cast<unsigned long long>(snap_.Plan.ToDelete.size()));
	ImGui::Text("阶段结果：复制 %llu/%llu · 备份 %llu/%llu · 删除 %llu/%llu（回收空目录 %llu）",
	            static_cast<unsigned long long>(snap_.Copy.Ok),
	            static_cast<unsigned long long>(snap_.Copy.Total),
	            static_cast<unsigned long long>(snap_.Backup.Ok),
	            static_cast<unsigned long long>(snap_.Backup.Total),
	            static_cast<unsigned long long>(snap_.Delete.Ok),
	            static_cast<unsigned long long>(snap_.Delete.Total),
	            static_cast<unsigned long long>(snap_.Delete.RemovedDirs));

	if (!snap_.Backup.BackupRoot.empty())
	{
		ImGui::TextDisabled("删除前备份在：%s", ToUtf8String(snap_.Backup.BackupRoot).c_str());
	}
	if (snap_.HasVerify)
	{
		ImGui::TextColored(snap_.VerifyPass ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.00f, 0.50f, 0.45f, 1.0f),
		                   "勾选项复核：%s", snap_.VerifyPass ? "通过" : "未通过");
	}

	auto drawErrors = [](const char* title, const PhaseReport& report)
	{
		if (report.Errors.empty())
		{
			return;
		}
		if (!ImGui::CollapsingHeader(title))
		{
			return;
		}
		for (const std::wstring& error : report.Errors)
		{
			ImGui::BulletText("%s", ToUtf8String(error).c_str());
		}
	};

	drawErrors("复制失败明细", snap_.Copy);
	drawErrors("备份失败明细", snap_.Backup);
	drawErrors("删除失败明细", snap_.Delete);
}

void AppUi::DrawConfigTab()
{
	if (reloadEditors_)
	{
		LoadEditorsFromConfig();
		reloadEditors_ = false;
	}

	// 当前备份目录：独立目录，与两侧文件夹无关
	const SyncConfig liveConfig = controller_.GetConfig();
	const std::wstring backupRoot = liveConfig.BackupDir.empty()
		? std::wstring(L"（尚未设置）")
		: JoinPath(TrimTrailingSlash(liveConfig.BackupDir), L"<时间戳>");

	ImGui::TextDisabled("当前配置文件：%s", ToUtf8String(configPath_).c_str());
	ImGui::TextDisabled("当前备份目录：%s", ToUtf8String(backupRoot).c_str());
	ImGui::TextDisabled("路径可以是任意两个文件夹，支持从资源管理器拖入；改完点「保存配置」才会生效。");
	ImGui::Separator();

	if (ImGui::Button("保存配置", ImVec2(110.0f, 0.0f)))
	{
		SaveConfigTo(configPath_, L"配置已保存；之前的差异结果已作废，请重新执行差异对比。");
	}
	ImGui::SameLine();
	if (ImGui::Button("导入配置…", ImVec2(110.0f, 0.0f)))
	{
		std::wstring picked;
		if (PickConfigFile(false, picked))
		{
			LoadConfigFrom(picked, L"已导入配置：" + picked);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("配置另存为…", ImVec2(130.0f, 0.0f)))
	{
		std::wstring picked;
		if (PickConfigFile(true, picked))
		{
			SaveConfigTo(picked, L"已另存为：" + picked);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("清空配置", ImVec2(110.0f, 0.0f)))
	{
		controller_.SetConfig(SyncConfig::Defaults());
		notice_ = L"已清空配置（点「保存配置」写入当前配置文件）。";
		reloadEditors_ = true;
	}

	ImGui::Separator();

	ImGui::TextDisabled("%s", ToUtf8String(LeafName(FromUtf8(editA_), L"文件夹 A")).c_str());
	ImGui::SetNextItemWidth(-170.0f);
	InputTextStd("##left", editA_);
	RecordFieldRect(aFieldRect_);
	ImGui::SameLine();
	if (ImGui::Button("浏览…##left", ImVec2(150.0f, 0.0f)))
	{
		std::wstring picked;
		if (PickFolder(picked))
		{
			editA_ = ToUtf8String(picked);
		}
	}

	ImGui::TextDisabled("%s", ToUtf8String(LeafName(FromUtf8(editB_), L"文件夹 B")).c_str());
	ImGui::SetNextItemWidth(-170.0f);
	InputTextStd("##right", editB_);
	RecordFieldRect(bFieldRect_);
	ImGui::SameLine();
	if (ImGui::Button("浏览…##right", ImVec2(150.0f, 0.0f)))
	{
		std::wstring picked;
		if (PickFolder(picked))
		{
			editB_ = ToUtf8String(picked);
		}
	}

	ImGui::TextDisabled("备份目录");
	ImGui::SetNextItemWidth(-170.0f);
	InputTextStd("##backup", editBackupDir_);
	RecordFieldRect(backupFieldRect_);
	ImGui::SameLine();
	if (ImGui::Button("浏览…##backup", ImVec2(150.0f, 0.0f)))
	{
		std::wstring picked;
		if (PickFolder(picked))
		{
			editBackupDir_ = ToUtf8String(picked);
		}
	}

	ImGui::Separator();
	const ImVec2 listSize(-FLT_MIN, 84.0f);

	ImGui::TextDisabled("任意层级排除的文件/文件夹");
	InputTextMultilineStd("##anyname", editAnyName_, listSize);

	ImGui::TextDisabled("仅顶层排除的文件/文件夹");
	InputTextMultilineStd("##rootname", editRootName_, listSize);

	ImGui::TextDisabled("排除的相对路径前缀");
	InputTextMultilineStd("##prefix", editPathPrefix_, listSize);

	ImGui::TextDisabled("仅顶层排除的扩展名");
	InputTextMultilineStd("##rootext", editRootExt_, listSize);

	if (controller_.GetConfig().HasNoExcludeRules())
	{
		ImGui::TextColored(ImVec4(1.00f, 0.75f, 0.35f, 1.0f),
		                   "当前没有任何排除规则");
	}

	ImGui::Separator();
	if (!notice_.empty())
	{
		ImGui::TextColored(ImVec4(0.60f, 0.85f, 1.00f, 1.0f), "%s", ToUtf8String(notice_).c_str());
	}

	const std::vector<std::wstring> problems = controller_.GetConfig().Validate();
	if (problems.empty())
	{
		ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "路径检查通过。");
		return;
	}

	for (const std::wstring& problem : problems)
	{
		ImGui::TextColored(ImVec4(1.00f, 0.50f, 0.45f, 1.0f), "%s", ToUtf8String(problem).c_str());
	}
}

void AppUi::DrawLogTab()
{
	if (ImGui::Button("打开日志目录"))
	{
		OpenWithShell(logDir_);
	}
	ImGui::SameLine();
	if (ImGui::Button("打开当前日志"))
	{
		OpenWithShell(snap_.LogPath);
	}
	ImGui::SameLine();
	ImGui::Checkbox("自动滚动", &autoScrollLog_);
	ImGui::SameLine();
	ImGui::TextDisabled("警告 %llu · 错误 %llu",
	                    static_cast<unsigned long long>(snap_.WarnCount),
	                    static_cast<unsigned long long>(snap_.ErrorCount));

	std::string text;
	for (const std::wstring& line : snap_.LogTail)
	{
		text += ToUtf8String(line);
		text += "\n";
	}

	ImGui::BeginChild("##logchild", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::TextUnformatted(text.c_str());
	if (autoScrollLog_)
	{
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
}

void AppUi::DrawConfirmModal()
{
	if (!ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	const DiffResult plan = SelectOnly(snap_.Diff);

	ImGui::Text("即将把 %s 的内容同步到 %s（仅勾选项）",
	            ToUtf8String(snap_.SrcLabel).c_str(),
	            ToUtf8String(snap_.DstLabel).c_str());
	ImGui::Separator();
	ImGui::TextColored(ImVec4(1.00f, 0.85f, 0.35f, 1.0f), "覆盖 / 新增 : %llu 个文件 (%s)",
	                   static_cast<unsigned long long>(plan.ToCopy.size()),
	                   ToUtf8String(FormatBytes(plan.CopyBytes)).c_str());
	ImGui::TextColored(ImVec4(1.00f, 0.50f, 0.45f, 1.0f), "删除        : %llu 个文件 (%s)",
	                   static_cast<unsigned long long>(plan.ToDelete.size()),
	                   ToUtf8String(FormatBytes(plan.DeleteBytes)).c_str());
	ImGui::Separator();
	ImGui::TextDisabled("删除项会先备份到 %s\\<时间戳>\\",
	                    ToUtf8String(controller_.GetConfig().BackupDir).c_str());

	ImGui::Spacing();
	if (ImGui::Button("执行", ImVec2(120.0f, 0.0f)))
	{
		ImGui::CloseCurrentPopup();
		controller_.StartExecute(false);
	}
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120.0f, 0.0f)))
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void AppUi::DrawPreflightModal()
{
	if (!ImGui::BeginPopupModal(kPreflightPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	ImGui::TextUnformatted("运行前检查发现问题：");
	ImGui::Separator();
	for (const std::wstring& issue : snap_.PreflightIssues)
	{
		ImGui::BulletText("%s", ToUtf8String(issue).c_str());
	}

	ImGui::Spacing();
	ImGui::TextDisabled("继续执行可能导致部分文件同步失败或写出不完整内容。");
	if (ImGui::Button("忽略并继续执行", ImVec2(160.0f, 0.0f)))
	{
		ImGui::CloseCurrentPopup();
		controller_.StartExecute(true);
	}
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120.0f, 0.0f)))
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void AppUi::RequestCompare()
{
	revealNotice_.clear();
	controller_.StartScan(direction_, true);
}

void AppUi::RequestExecute()
{
	requestConfirm_ = true;
}

void AppUi::QueueFolderDrop(int clientX, int clientY, const std::wstring& path)
{
	FolderDrop drop;
	drop.X = clientX;
	drop.Y = clientY;
	drop.Path = path;
	pendingDrops_.push_back(std::move(drop));
}

void AppUi::ProcessPendingDrops()
{
	if (pendingDrops_.empty())
	{
		return;
	}

	const std::vector<FolderDrop> drops = std::move(pendingDrops_);
	pendingDrops_.clear();

	auto hit = [](const FieldRect& rect, int x, int y)
	{
		if (!rect.Valid)
		{
			return false;
		}
		const float fx = static_cast<float>(x);
		const float fy = static_cast<float>(y);
		return fx >= rect.X0 && fx <= rect.X1 && fy >= rect.Y0 && fy <= rect.Y1;
	};

	for (const FolderDrop& drop : drops)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(std::filesystem::path(drop.Path), ec) || ec)
		{
			notice_ = L"只能拖入文件夹：" + drop.Path;
			forceTab_ = 1;
			continue;
		}

		if (hit(aFieldRect_, drop.X, drop.Y))
		{
			editA_ = ToUtf8String(drop.Path);
			notice_ = L"已把文件夹 A 的路径填为：" + drop.Path + L"，点「保存配置」生效。";
			continue;
		}

		if (hit(bFieldRect_, drop.X, drop.Y))
		{
			editB_ = ToUtf8String(drop.Path);
			notice_ = L"已把文件夹 B 的路径填为：" + drop.Path + L"，点「保存配置」生效。";
			continue;
		}

		if (hit(backupFieldRect_, drop.X, drop.Y))
		{
			editBackupDir_ = ToUtf8String(drop.Path);
			notice_ = L"已把备份目录填为：" + drop.Path + L"，点「保存配置」生效。";
			continue;
		}

		notice_ = L"请拖到具体的路径输入框上。";
		forceTab_ = 1;
	}
}

void AppUi::RecordFieldRect(FieldRect& rect) const
{
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	rect.X0 = min.x;
	rect.Y0 = min.y;
	rect.X1 = max.x;
	rect.Y1 = max.y;
	rect.Valid = true;
}

void AppUi::RebuildRows()
{
	rows_.clear();

	const std::wstring filter = ToLowerInvariant(FromUtf8(filterBuffer_));
	const bool hasFilter = !filter.empty();

	auto matches = [&filter, hasFilter](const std::wstring& rel)
	{
		if (!hasFilter)
		{
			return true;
		}
		return ToLowerInvariant(rel).find(filter) != std::wstring::npos;
	};

	if (rowsShowNew_ || rowsShowModified_)
	{
		for (DiffItem& item : snap_.Diff.ToCopy)
		{
			const bool kindShown = (item.Kind == ChangeKind::New) ? rowsShowNew_ : rowsShowModified_;
			if (!kindShown || !matches(item.Rel))
			{
				continue;
			}

			DisplayRow row;
			row.IsDelete = false;
			row.Item = &item;
			rows_.push_back(row);
		}
	}

	if (rowsShowDelete_)
	{
		for (DiffItem& item : snap_.Diff.ToDelete)
		{
			if (!matches(item.Rel))
			{
				continue;
			}

			DisplayRow row;
			row.IsDelete = true;
			row.Item = &item;
			rows_.push_back(row);
		}
	}
}

void AppUi::FlushSelection()
{
	if (!selectionDirty_)
	{
		return;
	}
	selectionDirty_ = false;

	std::vector<std::wstring> keys;
	keys.reserve(snap_.Diff.ToCopy.size() + snap_.Diff.ToDelete.size());
	for (const DiffItem& item : snap_.Diff.ToCopy)
	{
		if (item.Selected)
		{
			keys.push_back(item.Rel);
		}
	}
	for (const DiffItem& item : snap_.Diff.ToDelete)
	{
		if (item.Selected)
		{
			keys.push_back(item.Rel);
		}
	}

	controller_.SetSelection(keys);
}

void AppUi::ApplySelectionPolicy(int policy)
{
	if (rows_.empty())
	{
		return;
	}

	for (DisplayRow& row : rows_)
	{
		switch (policy)
		{
		case 0:		// 全选
			row.Item->Selected = true;
			break;
		case 1:		// 全不选
			row.Item->Selected = false;
			break;
		case 2:		// 反选
			row.Item->Selected = !row.Item->Selected;
			break;
		case 3:		// 仅新增 / 覆盖
			row.Item->Selected = !row.IsDelete;
			break;
		default:
			break;
		}
	}

	selectionDirty_ = true;
}

void AppUi::LoadEditorsFromConfig()
{
	const SyncConfig config = controller_.GetConfig();
	editA_ = ToUtf8String(config.RootA);
	editB_ = ToUtf8String(config.RootB);
	editBackupDir_ = ToUtf8String(config.BackupDir);
	editAnyName_ = JoinLines(config.ExcludeAnyName);
	editRootName_ = JoinLines(config.ExcludeRootName);
	editPathPrefix_ = JoinLines(config.ExcludePathPrefix);
	editRootExt_ = JoinLines(config.ExcludeRootExt);
}

void AppUi::BuildConfigFromEditors(SyncConfig& config) const
{
	config.RootA = TrimTrailingSlash(FromUtf8(editA_));
	config.RootB = TrimTrailingSlash(FromUtf8(editB_));
	config.BackupDir = TrimTrailingSlash(FromUtf8(editBackupDir_));
	if (config.BackupDir.empty())
	{
		config.BackupDir = SyncConfig::DefaultBackupDir();
	}

	config.ExcludeAnyName = ParseLines(editAnyName_);
	config.ExcludeRootName = ParseLines(editRootName_);
	config.ExcludePathPrefix = ParseLines(editPathPrefix_);
	config.ExcludeRootExt = ParseLines(editRootExt_);

	// 前缀与扩展名统一转小写；扩展名允许只写 sln，这里自动补前导点
	for (std::wstring& item : config.ExcludePathPrefix)
	{
		item = ToLowerInvariant(item);
	}
	for (std::wstring& item : config.ExcludeRootExt)
	{
		item = ToLowerInvariant(item);
		if (!item.empty() && item.front() != L'.')
		{
			item.insert(item.begin(), L'.');
		}
	}
}

bool AppUi::SaveConfigTo(const std::wstring& path, const std::wstring& message)
{
	SyncConfig config = controller_.GetConfig();
	BuildConfigFromEditors(config);
	controller_.SetConfig(config);

	const bool saved = config.Save(path);
	if (saved)
	{
		configPath_ = path;
		SaveLastConfigPath(settingsPath_, path);
	}

	notice_ = saved ? message : (L"写入配置文件失败：" + path);
	reloadEditors_ = true;
	return saved;
}

bool AppUi::LoadConfigFrom(const std::wstring& path, const std::wstring& message)
{
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec)
	{
		notice_ = L"配置文件不存在：" + path;
		return false;
	}

	SyncConfig config = SyncConfig::Defaults();
	config.Load(path);
	controller_.SetConfig(config);
	configPath_ = path;
	SaveLastConfigPath(settingsPath_, path);

	notice_ = message;
	reloadEditors_ = true;
	return true;
}

bool AppUi::PickFolder(std::wstring& out)
{
	IFileOpenDialog* dialog = nullptr;
	if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
	{
		return false;
	}

	DWORD options = 0;
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

	bool picked = false;
	if (SUCCEEDED(dialog->Show(static_cast<HWND>(hwnd_))))
	{
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)))
		{
			LPWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
			{
				out = path;
				picked = true;
				::CoTaskMemFree(path);
			}
			item->Release();
		}
	}

	dialog->Release();
	return picked;
}

bool AppUi::PickConfigFile(bool save, std::wstring& out)
{
	wchar_t buffer[MAX_PATH] = {};
	if (!out.empty())
	{
		wcsncpy_s(buffer, out.c_str(), _TRUNCATE);
	}

	OPENFILENAMEW dialog = {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = static_cast<HWND>(hwnd_);
	dialog.lpstrFilter = L"ProjectSync 配置 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0";
	dialog.lpstrFile = buffer;
	dialog.nMaxFile = MAX_PATH;
	dialog.lpstrDefExt = L"ini";
	dialog.Flags = save
		? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR)
		: (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR);

	const BOOL ok = save ? ::GetSaveFileNameW(&dialog) : ::GetOpenFileNameW(&dialog);
	if (!ok)
	{
		return false;
	}

	out = buffer;
	return true;
}
