#pragma once

#include <sqlite3.h>
#include <concepts>
#include <functional>
#include <unordered_dense.h>
#include <string>
#include <vector>

namespace knowledge_store {

template <typename T>
concept StringConvertibleParam = std::convertible_to<T, std::string> || std::integral<T>;

template <typename... Args>
concept StringConvertible = (StringConvertibleParam<Args> && ...);

class KnowledgeStore {
public:
    KnowledgeStore();
    ~KnowledgeStore();
    
    bool open(const std::string& db_path);
    void close();
    
    // Execute SQL with parameters
    int execute(const std::string& sql, const std::vector<std::string>& params = {});
    int execute(const std::string& sql, const std::string& p1);
    int execute(const std::string& sql, const std::string& p1, const std::string& p2);
    int execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3);
    int execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3, const std::string& p4);
    
    // Variadic execute for any number of string-convertible parameters
    template <typename... Args>
        requires StringConvertible<Args...>
    int execute(const std::string& sql, Args... params) {
        std::vector<std::string> param_vec;
        (..., param_vec.push_back(to_param(std::forward<Args>(params))));
        return execute(sql, param_vec);
    }
    
    // Query SQL and return results
    std::vector<std::vector<std::string>> query(const std::string& sql);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::vector<std::string>& params);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::string& p1);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::string& p1, const std::string& p2);

    // Variadic query for any number of string-convertible parameters
    template <typename... Args>
        requires StringConvertible<Args...>
    std::vector<std::vector<std::string>> query(const std::string& sql, Args... params) {
        std::vector<std::string> param_vec;
        (..., param_vec.push_back(to_param(std::forward<Args>(params))));
        return query(sql, param_vec);
    }
    
    // Get last error message
    std::string get_error() const;
    
    // Check if database is open
    bool is_open() const;

private:
    sqlite3* db_;
    std::string error_;
    ankerl::unordered_dense::map<std::string, sqlite3_stmt*> stmt_cache_;

    // Initialize schema
    bool init_schema();

    // Return a cached prepared statement for the SQL, resetting it and clearing
    // bindings. Statements are reset (never left mid-step) after each use, so
    // transactions (BEGIN/COMMIT) remain safe. Returns nullptr on failure.
    sqlite3_stmt* prepare_cached(const std::string& sql);

    static std::string to_param(const std::string& s) { return s; }
    static std::string to_param(const char* s) { return std::string(s); }
    template <typename T>
        requires std::integral<T>
    static std::string to_param(T v) { return std::to_string(v); }
};

}
