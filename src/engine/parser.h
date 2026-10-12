#pragma once

#include <string>
#include <vector>

namespace parser {

struct SymbolInfo {
    std::string name;
    std::string symbol_type;
    std::string signature;
    int start_line;
    int end_line;
    std::string scope;
    bool is_declaration;
    bool is_definition;
    bool is_exported;
};

struct SymbolDependency {
    std::string caller; // qualified name of the enclosing symbol ("" = file level)
    std::string callee; // name of the called symbol
    int line;
};

struct FileMetadata {
    std::string path;
    std::string language;
    std::string file_type;
    int size_bytes;
    int line_count;
    bool is_binary;
    std::vector<SymbolInfo> symbols;
    std::vector<std::string> dependencies;
    std::vector<SymbolDependency> symbol_deps;
};

// Parse a file and extract metadata
FileMetadata parse_file(const std::string& file_path, const std::string& language);

// Get language from file extension
std::string get_language_from_extension(const std::string& extension);

// Shared helpers used by the tree-sitter language parsers
namespace detail {

// Read raw file bytes; returns false if the file cannot be read.
bool read_file_content(const std::string& file_path, std::string& content);

// Populate path/language/file_type/size/line_count/binary from raw content.
void fill_basic_metadata(FileMetadata& metadata, const std::string& file_path, const std::string& content);

// Regex-based fallback (priority 3): basic class/function/import declarations.
std::vector<SymbolInfo> regex_extract_symbols(const std::string& content);
std::vector<std::string> regex_extract_dependencies(const std::string& content);

}

}
