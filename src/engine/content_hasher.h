#pragma once

#include <string>

namespace content_hasher {

// Compute SHA-256 hash of file contents
std::string compute_file_hash(const std::string& file_path);

// Compute SHA-256 hash of string content
std::string compute_string_hash(const std::string& content);

}
