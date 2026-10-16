#include "core/Logger.h"

#include "core/StringUtil.h"

#include <filesystem>

bool Logger::Open(const std::wstring& logDir)
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(logDir), ec);
	if (ec)
	{
		return false;
	}

	path_ = JoinPath(logDir, L"sync-" + TimeStamp() + L".log");
	file_.open(std::filesystem::path(path_), std::ios::binary | std::ios::trunc);
	if (!file_.is_open())
	{
		path_.clear();
		return false;
	}

	// 带 BOM：与旧 PowerShell 版日志一致，记事本打开不会乱码
	const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
	file_.write(reinterpret_cast<const char*>(bom), sizeof(bom));
	file_.flush();

	tail_.clear();
	warnCount_ = 0;
	errorCount_ = 0;
	return true;
}

void Logger::Close()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (file_.is_open())
	{
		file_.flush();
		file_.close();
	}
}

void Logger::Log(const std::wstring& text)
{
	WriteLine(L"", text);
}

void Logger::Step(const std::wstring& text)
{
	WriteLine(L"  ", text);
}

void Logger::Ok(const std::wstring& text)
{
	WriteLine(L"[OK] ", text);
}

void Logger::Warn(const std::wstring& text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	++warnCount_;
	WriteLineLocked(L"[警告] ", text);
}

void Logger::Error(const std::wstring& text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	++errorCount_;
	WriteLineLocked(L"[错误] ", text);
}

uint64_t Logger::WarnCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return warnCount_;
}

uint64_t Logger::ErrorCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return errorCount_;
}

std::wstring Logger::FilePath() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return path_;
}

std::vector<std::wstring> Logger::Tail(size_t maxLines) const
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::vector<std::wstring> out;
	const size_t count = (tail_.size() < maxLines) ? tail_.size() : maxLines;
	out.reserve(count);
	for (size_t i = tail_.size() - count; i < tail_.size(); ++i)
	{
		out.push_back(tail_[i]);
	}
	return out;
}

void Logger::WriteLine(const std::wstring& prefix, const std::wstring& text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	WriteLineLocked(prefix, text);
}

void Logger::WriteLineLocked(const std::wstring& prefix, const std::wstring& text)
{
	const std::wstring line = L"[" + TimeOnly() + L"] " + prefix + text;

	if (file_.is_open())
	{
		const std::string utf8 = ToUtf8(line) + "\r\n";
		file_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
		file_.flush();
	}

	tail_.push_back(line);
	if (tail_.size() > kTailCapacity)
	{
		tail_.pop_front();
	}
}
