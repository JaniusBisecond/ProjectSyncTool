#pragma once

#include "core/SyncTypes.h"

#include <string>
#include <vector>

// 同步配置。默认不提供路径与排除规则（由界面填写或导入配置文件），
// 备份目录默认取程序同级目录下的 _sync-backup。
class SyncConfig
{
public:
	std::wstring RootA;				// 文件夹 A
	std::wstring RootB;				// 文件夹 B
	std::wstring BackupDir;			// 备份目录（完整路径，独立于两侧文件夹）

	// 任意层级排除的文件/文件夹（名字相等即排除）
	std::vector<std::wstring> ExcludeAnyName;
	// 仅顶层排除的文件/文件夹
	std::vector<std::wstring> ExcludeRootName;
	// 排除的相对路径前缀（小写）
	std::vector<std::wstring> ExcludePathPrefix;
	// 仅顶层排除的扩展名（小写，含点）
	std::vector<std::wstring> ExcludeRootExt;

	// 默认配置：路径与排除规则为空，备份目录取程序同级目录
	static SyncConfig Defaults();

	// 程序同级目录下的默认备份目录
	static std::wstring DefaultBackupDir();

	// 读取 ini；文件不存在时写出一份默认配置，返回是否可用
	bool Load(const std::wstring& iniPath);
	bool Save(const std::wstring& iniPath) const;

	// 返回配置问题清单（空表示通过）
	std::vector<std::wstring> Validate() const;

	// 是否一条排除规则都没有
	bool HasNoExcludeRules() const;
};

// 取路径末级目录名，用作界面标签；取不到时返回 fallback
std::wstring LeafName(const std::wstring& path, const std::wstring& fallback);

// 同步方向解析出的源 / 目标
struct DirectionTargets
{
	std::wstring Src;
	std::wstring Dst;
	std::wstring SrcLabel;
	std::wstring DstLabel;
};

DirectionTargets ResolveDirection(const SyncConfig& config, SyncDirection direction);

// exe 旁的默认配置 / 设置文件
std::wstring DefaultConfigPath();
std::wstring DefaultSettingsPath();

// 记住上次使用的配置文件，下次启动自动载入
bool LoadLastConfigPath(const std::wstring& settingsPath, std::wstring& outConfigPath);
bool SaveLastConfigPath(const std::wstring& settingsPath, const std::wstring& configPath);
