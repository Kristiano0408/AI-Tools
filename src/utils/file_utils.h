#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace file_utils {

// Read entire file contents
std::string read_file(const std::string& path);

// Write contents to a file
bool write_file(const std::string& path, const std::string& content);

// Get file size
std::uintmax_t get_file_size(const std::string& path);

// Get file modification time
std::time_t get_modification_time(const std::string& path);

// Read specific lines from a file
std::vector<std::string> read_lines(const std::string& path, int start_line = 1, int end_line = -1);

// Check if a file is binary
bool is_binary(const std::string& path);

// Walk directory recursively
std::vector<std::string> walk_directory(const std::string& dir, bool follow_symlinks = false);

}
