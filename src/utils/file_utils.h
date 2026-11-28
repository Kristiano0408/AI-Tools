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

// Extract a 1-based inclusive line range from an ALREADY-READ buffer.
//
// This is the snapshot counterpart of read_lines(): it never touches the
// filesystem, so a caller that already holds the bytes it hashed can derive a
// symbol's region from those exact bytes (IDX-03). Lines are delimited by
// '\n', the same boundary tree-sitter reports for start_line/end_line, so the
// slice covers exactly the region the parser attributed to the symbol. The
// newline TERMINATING end_line is not included: the slice must not depend on
// what follows the region. Returns "" when the range does not exist
// (start_line past the end of the content).
std::string slice_lines(const std::string& content, int start_line, int end_line = -1);

// Check if a file is binary
bool is_binary(const std::string& path);

// Walk directory recursively
std::vector<std::string> walk_directory(const std::string& dir, bool follow_symlinks = false);

}
