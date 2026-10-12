#include "file_utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <vector>
#include <string>
#include <ctime>

namespace file_utils {

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    
    file.seekg(0, std::ios::end);
    std::size_t size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    std::string content(size, '\0');
    file.read(&content[0], size);
    return content;
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    
    file.write(content.c_str(), content.size());
    return file.good();
}

std::uintmax_t get_file_size(const std::string& path) {
    std::filesystem::path p(path);
    return std::filesystem::file_size(p);
}

std::time_t get_modification_time(const std::string& path) {
    std::filesystem::path p(path);
    auto ftime = std::filesystem::last_write_time(p);
    // Convert file_time to system_clock time (C++20)
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

std::vector<std::string> read_lines(const std::string& path, int start_line, int end_line) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file.is_open()) return lines;
    
    std::string line;
    int current_line = 1;
    
    while (std::getline(file, line)) {
        if (current_line >= start_line && (end_line == -1 || current_line <= end_line)) {
            lines.push_back(line);
        }
        if (end_line != -1 && current_line > end_line) break;
        current_line++;
    }
    
    return lines;
}

bool is_binary(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return true;
    
    char buffer[1024];
    file.read(buffer, sizeof(buffer));
    
    for (std::streamsize i = 0; i < file.gcount(); i++) {
        if (buffer[i] == 0) return true; // Null byte found
    }
    
    return false;
}

std::vector<std::string> walk_directory(const std::string& dir, bool follow_symlinks) {
    std::vector<std::string> files;
    
    for (const auto& entry : std::filesystem::directory_iterator(dir, 
            follow_symlinks ? std::filesystem::directory_options::follow_directory_symlink : std::filesystem::directory_options::none)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path().string());
        } else if (entry.is_directory()) {
            // Skip hidden directories and common non-source directories
            std::string name = entry.path().filename().string();
            if (name[0] != '.' && name != "build" && name != "cmake-build" && 
                name != ".git" && name != "node_modules" && name != "venv" &&
                name != ".venv" && name != "__pycache__" && name != ".tox") {
                auto subfiles = walk_directory(entry.path().string(), follow_symlinks);
                files.insert(files.end(), subfiles.begin(), subfiles.end());
            }
        }
    }
    
    return files;
}

}
