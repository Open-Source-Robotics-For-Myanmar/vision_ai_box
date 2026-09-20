#include "Logger.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{
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
}

Logger::Logger()
{
	namespace fs = std::filesystem;
	fs::create_directories("logs");

	const auto now = std::chrono::system_clock::now();
	const auto time = std::chrono::system_clock::to_time_t(now);
	std::tm local_time{};
	localtime_r(&time, &local_time);

	std::ostringstream filename;
	filename << "logs/log_"
		 << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S")
		 << ".log";
	session_log_path_ = filename.str();

	log_file_.open(session_log_path_, std::ios::out | std::ios::app);
	if (!log_file_) {
		std::cerr << "[LOGGER] Unable to create session log file: " << session_log_path_ << std::endl;
	}

	log(LogLevel::INFO, "SYSTEM", "Session log started: " + session_log_path_);
}

Logger::~Logger()
{
	if (log_file_.is_open()) {
		log_file_.flush();
		log_file_.close();
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

	if (log_file_.is_open()) {
		log_file_ << line << '\n';
		log_file_.flush();
	}
}

std::vector<std::string> Logger::read_all_logs() const
{
	std::lock_guard lock(mutex_);
	std::ifstream input(session_log_path_);
	std::vector<std::string> lines;
	if (!input.is_open()) {
		return lines;
	}

	std::string line;
	while (std::getline(input, line)) {
		lines.push_back(line);
	}

	return lines;
}