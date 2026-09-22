#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
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
	// The three helpers below need mutex_ held, or a caller that has not yet
	// shared the logger with other threads.
	void open_log_file();
	void rotate_if_needed(std::size_t incoming_bytes);
	void prune_old_logs();

	mutable std::mutex mutex_;
	std::filesystem::path log_directory_;
	std::string session_stamp_;
	std::uint32_t file_index_{0};
	std::string session_log_path_;
	mutable std::ofstream log_file_;
	std::uintmax_t current_bytes_{0};
	std::uintmax_t max_bytes_;
	std::size_t max_files_;
	std::chrono::steady_clock::time_point last_flush_{};
};
