#pragma once

#include <cstdint>
#include <string>

// 大小写无关相等（不变量区域，等价于 PowerShell 的 -eq / -contains）
bool IEquals(const std::wstring& a, const std::wstring& b);

// 转小写（不变量区域，等价于 ToLowerInvariant）
std::wstring ToLowerInvariant(const std::wstring& text);

// 前缀判断（大小写敏感）
bool StartsWith(const std::wstring& text, const std::wstring& prefix);

// UTF-16 <-> UTF-8；ImGui 只接受 UTF-8
std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

// 体积显示：1.24 GB / 3.50 MB / 512 B
std::wstring FormatBytes(uint64_t bytes);

// printf 风格的宽字符格式化
std::wstring FormatW(const wchar_t* format, ...);

// 本地时间：yyyyMMdd-HHmmss / HH:mm:ss
std::wstring TimeStamp();
std::wstring TimeOnly();

// GetLastError 对应的可读文本
std::wstring LastErrorMessage(unsigned long code);

// 路径拼接 root + "\" + rel（rel 为空时返回 root）
std::wstring JoinPath(const std::wstring& root, const std::wstring& rel);

// 去掉结尾的 \ 与 /
std::wstring TrimTrailingSlash(const std::wstring& path);

// 当前进程 exe 所在目录
std::wstring GetExeDirectory();
