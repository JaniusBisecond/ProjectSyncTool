#include "core/StringUtil.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <vector>

bool IEquals(const std::wstring& a, const std::wstring& b)
{
	return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
	                              b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ToLowerInvariant(const std::wstring& text)
{
	if (text.empty())
	{
		return text;
	}

	const int needed = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
	                                   text.c_str(), static_cast<int>(text.size()),
	                                   nullptr, 0, nullptr, nullptr, 0);
	if (needed <= 0)
	{
		std::wstring fallback = text;
		for (wchar_t& c : fallback)
		{
			c = static_cast<wchar_t>(::towlower(c));
		}
		return fallback;
	}

	std::wstring out(static_cast<size_t>(needed), L'\0');
	const int written = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
	                                    text.c_str(), static_cast<int>(text.size()),
	                                    out.data(), needed, nullptr, nullptr, 0);
	if (written <= 0)
	{
		return text;
	}

	out.resize(static_cast<size_t>(written));
	while (!out.empty() && out.back() == L'\0')
	{
		out.pop_back();
	}
	return out;
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix)
{
	if (prefix.size() > text.size())
	{
		return false;
	}
	return std::wmemcmp(text.data(), prefix.data(), prefix.size()) == 0;
}

std::string ToUtf8(const std::wstring& text)
{
	if (text.empty())
	{
		return std::string();
	}

	const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
	                                         nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
	{
		return std::string();
	}

	std::string out(static_cast<size_t>(needed), '\0');
	const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
	                                          out.data(), needed, nullptr, nullptr);
	if (written <= 0)
	{
		return std::string();
	}
	out.resize(static_cast<size_t>(written));
	return out;
}

std::wstring FromUtf8(const std::string& text)
{
	if (text.empty())
	{
		return std::wstring();
	}

	const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (needed <= 0)
	{
		return std::wstring();
	}

	std::wstring out(static_cast<size_t>(needed), L'\0');
	const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
	if (written <= 0)
	{
		return std::wstring();
	}
	out.resize(static_cast<size_t>(written));
	return out;
}

std::wstring FormatBytes(uint64_t bytes)
{
	constexpr uint64_t kKB = 1024ull;
	constexpr uint64_t kMB = kKB * 1024ull;
	constexpr uint64_t kGB = kMB * 1024ull;

	if (bytes >= kGB)
	{
		return FormatW(L"%.2f GB", static_cast<double>(bytes) / static_cast<double>(kGB));
	}
	if (bytes >= kMB)
	{
		return FormatW(L"%.2f MB", static_cast<double>(bytes) / static_cast<double>(kMB));
	}
	if (bytes >= kKB)
	{
		return FormatW(L"%.1f KB", static_cast<double>(bytes) / static_cast<double>(kKB));
	}
	return FormatW(L"%llu B", static_cast<unsigned long long>(bytes));
}

std::wstring FormatW(const wchar_t* format, ...)
{
	wchar_t buffer[4096] = {};
	va_list args;
	va_start(args, format);
	const int written = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
	va_end(args);

	if (written <= 0)
	{
		return std::wstring();
	}
	return std::wstring(buffer, static_cast<size_t>(written));
}

std::wstring TimeStamp()
{
	SYSTEMTIME st = {};
	::GetLocalTime(&st);
	return FormatW(L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::wstring TimeOnly()
{
	SYSTEMTIME st = {};
	::GetLocalTime(&st);
	return FormatW(L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
}

std::wstring LastErrorMessage(unsigned long code)
{
	if (code == 0)
	{
		return L"未知错误";
	}

	LPWSTR buffer = nullptr;
	const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                                      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	                                      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

	std::wstring message;
	if (length != 0 && buffer != nullptr)
	{
		message.assign(buffer, length);
		while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
		{
			message.pop_back();
		}
	}
	if (buffer != nullptr)
	{
		::LocalFree(buffer);
	}

	if (message.empty())
	{
		message = FormatW(L"错误码 %lu", code);
	}
	return message;
}

std::wstring JoinPath(const std::wstring& root, const std::wstring& rel)
{
	if (rel.empty())
	{
		return root;
	}

	const std::wstring trimmed = TrimTrailingSlash(root);
	if (trimmed.empty())
	{
		return rel;
	}
	return trimmed + L"\\" + rel;
}

std::wstring TrimTrailingSlash(const std::wstring& path)
{
	std::wstring out = path;
	while (!out.empty() && (out.back() == L'\\' || out.back() == L'/'))
	{
		out.pop_back();
	}
	return out;
}

std::wstring GetExeDirectory()
{
	wchar_t buffer[MAX_PATH] = {};
	const DWORD length = ::GetModuleFileNameW(nullptr, buffer, _countof(buffer));
	if (length == 0)
	{
		return std::wstring();
	}

	const std::wstring full(buffer, static_cast<size_t>(length));
	const size_t pos = full.find_last_of(L"\\/");
	if (pos == std::wstring::npos)
	{
		return std::wstring();
	}
	return full.substr(0, pos);
}
