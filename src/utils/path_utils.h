#pragma once

#include <string>
#include <filesystem>

namespace path_utils {

// Validate that a path is within a repository root
bool is_within_repository(const std::string& repo_root, const std::string& path);

// Resolve a path relative to a repository root
std::string resolve_path(const std::string& repo_root, const std::string& relative_path);

// Get the absolute path of a file
std::string get_absolute_path(const std::string& path);

// Check if a file exists
bool file_exists(const std::string& path);

// Get file extension (without dot)
std::string get_extension(const std::string& path);

// Get file basename
std::string get_basename(const std::string& path);

// Normalize path (remove .., ., etc.)
std::string normalize_path(const std::string& path);

}
