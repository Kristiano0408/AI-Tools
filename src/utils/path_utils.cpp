#include "path_utils.h"
#include <algorithm>
#include <fstream>
#include <sys/stat.h>

namespace path_utils {

bool is_within_repository(const std::string& repo_root, const std::string& path) {
    std::error_code ec;
    std::string abs_repo = std::filesystem::weakly_canonical(repo_root, ec).string();
    if (ec) return false;
    std::string abs_path = std::filesystem::weakly_canonical(path, ec).string();
    if (ec) return false;
    if (abs_path.size() < abs_repo.size()) return false;
    return abs_path.compare(0, abs_repo.size(), abs_repo) == 0 &&
           (abs_path.size() == abs_repo.size() || abs_path[abs_repo.size()] == '/');
}

std::string resolve_path(const std::string& repo_root, const std::string& relative_path) {
    return std::filesystem::path(repo_root) / relative_path;
}

std::string get_absolute_path(const std::string& path) {
    return std::filesystem::absolute(path);
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string get_extension(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    return path.substr(dot + 1);
}

std::string get_basename(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string normalize_path(const std::string& path) {
    return std::filesystem::path(path).string();
}

}
