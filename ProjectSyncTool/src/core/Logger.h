#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// 会话日志：写盘（UTF-8）并在内存里保留末尾若干行供 GUI 显示。
// 线程安全：核心层 worker 线程写，UI 线程读。
class Logger
{
public:
	// 在 logDir 下创建 sync-<时间戳>.log；失败返回 false
	bool Open(const std::wstring& logDir);
	void Close();

	void Log(const std::wstring& text);
	void Step(const std::wstring& text);
	void Ok(const std::wstring& text);
	void Warn(const std::wstring& text);
	void Error(const std::wstring& text);

	uint64_t WarnCount() const;
	uint64_t ErrorCount() const;
	std::wstring FilePath() const;

	// 末尾 maxLines 行（含时间前缀）
	std::vector<std::wstring> Tail(size_t maxLines) const;

private:
	void WriteLine(const std::wstring& prefix, const std::wstring& text);
	void WriteLineLocked(const std::wstring& prefix, const std::wstring& text);	// 调用方需已持有 mutex_

	mutable std::mutex mutex_;
	std::ofstream file_;
	std::deque<std::wstring> tail_;
	std::wstring path_;
	uint64_t warnCount_ = 0;
	uint64_t errorCount_ = 0;

	// 内存里保留的日志行数上限
	static constexpr size_t kTailCapacity = 2000;
};
