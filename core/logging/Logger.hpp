#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

enum class LogLevel
{
	INFO,
	WARN,
	ERROR,
	DEBUG
};

class Logger
{
public:
	Logger();
	~Logger();

	void log(LogLevel level, const std::string& category, const std::string& message);
	std::vector<std::string> read_all_logs() const;

private:
	mutable std::mutex mutex_;
	std::string session_log_path_;
	std::ofstream log_file_;
};