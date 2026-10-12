#include "parser.h"
#include "parser/cpp_parser.h"
#include "parser/cuda_parser.h"
#include "parser/python_parser.h"

#include <fstream>
#include <iterator>
#include <algorithm>
#include <regex>
#include <sstream>

namespace parser {

FileMetadata parse_file(const std::string& file_path, const std::string& language) {
    if (language == "c" || language == "cpp") {
        return parse_cpp(file_path);
    }
    if (language == "cuda") {
        return parse_cuda(file_path);
    }
    if (language == "python") {
        return parse_python(file_path);
    }

    // Unsupported language: regex-based fallback (priority 3) — basic structural
    // patterns only, no semantic understanding.
    FileMetadata metadata;
    std::string content;
    if (!detail::read_file_content(file_path, content)) {
        metadata.path = file_path;
        metadata.language = language;
        return metadata;
    }
    detail::fill_basic_metadata(metadata, file_path, content);
    metadata.language = language;
    metadata.symbols = detail::regex_extract_symbols(content);
    metadata.dependencies = detail::regex_extract_dependencies(content);
    return metadata;
}

std::string get_language_from_extension(const std::string& extension) {
    if (extension == "c") return "c";
    if (extension == "cpp" || extension == "cc" || extension == "cxx" || extension == "c++" ||
        extension == "h" || extension == "hpp" || extension == "hxx") return "cpp";
    if (extension == "cu" || extension == "cuh") return "cuda";
    if (extension == "py") return "python";
    return "unknown";
}

namespace detail {

bool read_file_content(const std::string& file_path, std::string& content) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

void fill_basic_metadata(FileMetadata& metadata, const std::string& file_path, const std::string& content) {
    metadata.path = file_path;

    // Binary detection: NUL byte in the first chunk
    metadata.is_binary = false;
    size_t check = std::min(content.size(), size_t(8192));
    for (size_t i = 0; i < check; ++i) {
        if (content[i] == '\0') {
            metadata.is_binary = true;
            break;
        }
    }

    // Line count
    metadata.line_count = 0;
    for (char c : content) {
        if (c == '\n') {
            metadata.line_count++;
        }
    }
    if (!content.empty() && content.back() != '\n') {
        metadata.line_count++;
    }

    // File type from extension
    std::string ext = file_path.substr(file_path.rfind('.') + 1);
    if (ext == "h" || ext == "hpp" || ext == "hxx" || ext == "h++" || ext == "hh") {
        metadata.file_type = "header";
    } else if (ext == "cu" || ext == "cuh") {
        metadata.file_type = "cuda";
    } else if (ext == "c" || ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" || ext == "py") {
        metadata.file_type = "source";
    } else {
        metadata.file_type = "other";
    }

    metadata.size_bytes = static_cast<int>(content.size());
}

std::vector<SymbolInfo> regex_extract_symbols(const std::string& content) {
    std::vector<SymbolInfo> symbols;

    // Basic structural patterns: class / function / def / struct / interface declarations
    static const std::regex patterns[] = {
        std::regex("(^|\\s)(class|struct|interface|enum)\\s+([A-Za-z_][A-Za-z0-9_]*)"),
        std::regex("(^|\\s)(function|def|func|fn)\\s+([A-Za-z_][A-Za-z0-9_]*)"),
    };

    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line)) {
        line_no++;
        for (const auto& re : patterns) {
            std::smatch m;
            if (std::regex_search(line, m, re)) {
                SymbolInfo sym;
                sym.name = m[3].str();
                std::string kw = m[2].str();
                sym.symbol_type = (kw == "class" || kw == "interface") ? "class"
                                : (kw == "struct") ? "struct"
                                : (kw == "enum") ? "enum" : "function";
                sym.signature = line;
                sym.start_line = line_no;
                sym.end_line = line_no;
                sym.is_declaration = true;
                symbols.push_back(sym);
                break;
            }
        }
    }
    return symbols;
}

std::vector<std::string> regex_extract_dependencies(const std::string& content) {
    std::vector<std::string> deps;
    static const std::regex import_re("(^|\\s)(import|include|require|use|from)\\s+[<\"']?([^\"'\\s<>;]+)[\"'>]?", std::regex::ECMAScript);

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_search(line, m, import_re)) {
            deps.push_back(m[3].str());
        }
    }
    return deps;
}

}

}
