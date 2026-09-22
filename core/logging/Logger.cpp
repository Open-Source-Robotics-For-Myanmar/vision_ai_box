#include "Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace
{
constexpr std::uintmax_t kDefaultMaxBytes = 2 * 1024 * 1024;
constexpr std::size_t kDefaultMaxFiles = 5;

// /api/logs materialises the whole reply in memory before sending, so only the
// tail of the active file is served.
constexpr std::size_t kMaxReturnedLines = 2000;

const char* level_name(LogLevel level)
{
	switch (level) {
	case LogLevel::INFO:  return "INFO";
	case LogLevel::WARN:  return "WARN";
	case LogLevel::ERROR: return "ERROR";
	case LogLevel::DEBUG: return "DEBUG";
	}

	return "UNKNOWN";
}

std::string format_timestamp(const std::tm& time_info, std::chrono::milliseconds milliseconds)
{
	std::ostringstream stamp;
	stamp << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S")
		  << '.'
		  << std::setw(3) << std::setfill('0') << milliseconds.count();
	return stamp.str();
}

std::uintmax_t configured_size(const char* name, std::uintmax_t fallback)
{
	const char* value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}

	try {
		const unsigned long long parsed = std::stoull(value);
		if (parsed > 0) {
			return static_cast<std::uintmax_t>(parsed);
		}
	} catch (const std::exception&) {
	}

	return fallback;
}
}

Logger::Logger()
	: log_directory_("logs"),
	  max_bytes_(configured_size("VISION_AI_BOX_LOG_MAX_BYTES", kDefaultMaxBytes)),
	  max_files_(std::max<std::size_t>(1,
		  static_cast<std::size_t>(configured_size("VISION_AI_BOX_LOG_MAX_FILES", kDefaultMaxFiles))))
{
	std::error_code directory_error;
	std::filesystem::create_directories(log_directory_, directory_error);

	const auto now = std::chrono::system_clock::now();
	const auto time = std::chrono::system_clock::to_time_t(now);
	std::tm local_time{};
	localtime_r(&time, &local_time);

	std::ostringstream stamp;
	stamp << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S");
	session_stamp_ = stamp.str();

	open_log_file();

	log(LogLevel::INFO, "SYSTEM", "Session log started: " + session_log_path_);
}

Logger::~Logger()
{
	std::lock_guard lock(mutex_);
	if (log_file_.is_open()) {
		log_file_.flush();
		log_file_.close();
	}
}

void Logger::open_log_file()
{
	if (log_file_.is_open()) {
		log_file_.flush();
		log_file_.close();
	}

	std::ostringstream filename;
	filename << "log_" << session_stamp_ << '_'
			 << std::setw(3) << std::setfill('0') << file_index_ << ".log";
	++file_index_;

	session_log_path_ = (log_directory_ / filename.str()).string();
	current_bytes_ = 0;
	last_flush_ = std::chrono::steady_clock::now();

	log_file_.open(session_log_path_, std::ios::out | std::ios::trunc);
	if (!log_file_) {
		std::cerr << "[LOGGER] Unable to create session log file: " << session_log_path_ << std::endl;
	}

	prune_old_logs();
}

void Logger::rotate_if_needed(std::size_t incoming_bytes)
{
	if (current_bytes_ + incoming_bytes <= max_bytes_) {
		return;
	}

	open_log_file();
}

void Logger::prune_old_logs()
{
	std::error_code iteration_error;
	std::vector<std::filesystem::path> existing;
	for (const auto& entry : std::filesystem::directory_iterator(log_directory_, iteration_error)) {
		std::error_code status_error;
		if (entry.is_regular_file(status_error) && !status_error &&
			entry.path().extension() == ".log") {
			existing.push_back(entry.path());
		}
	}

	if (iteration_error || existing.size() <= max_files_) {
		return;
	}

	// Oldest first, so the newest max_files_ entries survive. The file that was
	// just opened is the newest, which keeps it out of the removal range.
	std::sort(existing.begin(), existing.end(), [](const auto& left, const auto& right) {
		std::error_code left_error;
		std::error_code right_error;
		return std::filesystem::last_write_time(left, left_error) <
			   std::filesystem::last_write_time(right, right_error);
	});

	const std::size_t removable = existing.size() - max_files_;
	for (std::size_t i = 0; i < removable; ++i) {
		std::error_code remove_error;
		std::filesystem::remove(existing[i], remove_error);
	}
}

void Logger::log(LogLevel level, const std::string& category, const std::string& message)
{
	const auto now = std::chrono::system_clock::now();
	const auto time = std::chrono::system_clock::to_time_t(now);
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()) % 1000;

	std::tm local_time{};
	localtime_r(&time, &local_time);

	const std::string line =
		"[" + format_timestamp(local_time, milliseconds) + "] [" +
		level_name(level) + "] [" + category + "] " + message;

	std::lock_guard lock(mutex_);

	std::cout << line << '\n';
	std::cout.flush();

	recent_lines_.push_back(line);
	if (recent_lines_.size() > kMaxReturnedLines) {
		recent_lines_.pop_front();
	}
	++next_line_index_;

	if (!log_file_.is_open()) {
		return;
	}

	const std::size_t line_bytes = line.size() + 1;
	rotate_if_needed(line_bytes);
	log_file_ << line << '\n';
	current_bytes_ += line_bytes;

	// Flushing every line costs one flash write per log call, which at frame
	// rate wears the device out. Problems still flush immediately so a crash
	// keeps the records explaining it.
	const auto flush_now = std::chrono::steady_clock::now();
	if (level == LogLevel::WARN || level == LogLevel::ERROR ||
		flush_now - last_flush_ >= std::chrono::seconds(1)) {
		log_file_.flush();
		last_flush_ = flush_now;
	}
}

std::vector<std::string> Logger::read_all_logs() const
{
	std::lock_guard lock(mutex_);
	return {recent_lines_.begin(), recent_lines_.end()};
}

std::vector<std::string> Logger::read_logs_after(std::uint64_t& cursor) const
{
	std::lock_guard lock(mutex_);
	const std::uint64_t first = next_line_index_ - recent_lines_.size();
	if (cursor < first) {
		cursor = first;
	}

	std::vector<std::string> lines;
	for (std::uint64_t index = cursor; index < next_line_index_; ++index) {
		lines.push_back(recent_lines_[static_cast<std::size_t>(index - first)]);
	}
	cursor = next_line_index_;
	return lines;
}
