// Minimal test runner for the tree-sitter based parsers (C, C++, Python).
// Verifies symbol extraction and dependency extraction end-to-end.

#include "engine/parser.h"
#include "engine/parser/cpp_parser.h"
#include "engine/parser/python_parser.h"
#include "engine/parser/cuda_parser.h"
#include "engine/knowledge_store.h"
#include "engine/indexer.h"
#include "engine/symbol_index.h"
#include "engine/dependency_graph.h"
#include "engine/summary_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  PASS: " << what << "\n";
    } else {
        std::cout << "  FAIL: " << what << "\n";
        ++failures;
    }
}

int run_parser_tests();

static bool has_symbol(const parser::FileMetadata& md, const std::string& name, const std::string& type) {
    for (const auto& s : md.symbols) {
        if (s.name == name && s.symbol_type == type) return true;
    }
    return false;
}

static bool has_dep(const parser::FileMetadata& md, const std::string& dep) {
    for (const auto& d : md.dependencies) {
        if (d == dep) return true;
    }
    return false;
}

static bool has_call(const parser::FileMetadata& md, const std::string& caller, const std::string& callee) {
    for (const auto& d : md.symbol_deps) {
        if (d.caller == caller && d.callee == callee) return true;
    }
    return false;
}

static std::string write_fixture(const std::string& dir, const std::string& name, const std::string& content) {
    std::filesystem::path p = std::filesystem::path(dir) / name;
    std::ofstream f(p);
    f << content;
    f.close();
    return p.string();
}

int run_parser_tests() {
    failures = 0;
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "ai-tools-parser-tests";
    fs::create_directories(tmp);

    // ---- C++ ----
    std::cout << "[cpp_parser: C++]\n";
    std::string cpp = write_fixture(tmp, "widget.cpp", R"cpp(
#include "widget.h"
#include <string>

namespace app {

class Widget : public Base {
public:
    void draw(int x, int y) const;
    int resize(int w, int h) {
        return w * h;
    }
private:
    int size_;
};

struct Point {
    int x;
    int y;
};

enum Color { RED, GREEN };

void free_function(int a, int b) {
    (void)a; (void)b;
}

} // namespace app
)cpp");

    parser::FileMetadata md = parser::parse_cpp(cpp);
    check(md.language == "cpp", "language is cpp");
    check(md.file_type == "source", "file_type is source");
    check(md.line_count > 10, "line_count > 10");
    check(has_symbol(md, "app", "namespace"), "namespace app found");
    check(has_symbol(md, "Widget", "class"), "class Widget found");
    check(has_symbol(md, "Point", "struct"), "struct Point found");
    check(has_symbol(md, "Color", "enum"), "enum Color found");
    check(has_symbol(md, "draw", "function"), "method draw found");
    check(has_symbol(md, "resize", "function"), "method resize found");
    check(has_symbol(md, "free_function", "function"), "free_function found");
    check(has_dep(md, "widget.h"), "include widget.h found");
    check(has_dep(md, "string"), "include <string> found");

    // Declaration vs definition
    bool draw_is_decl = false, resize_is_def = false;
    for (const auto& s : md.symbols) {
        if (s.name == "draw") draw_is_decl = s.is_declaration && !s.is_definition;
        if (s.name == "resize") resize_is_def = s.is_definition;
    }
    check(draw_is_decl, "draw marked as declaration");
    check(resize_is_def, "resize marked as definition");

    // Scope tracking
    bool widget_scoped = false;
    for (const auto& s : md.symbols) {
        if (s.name == "resize" && s.scope == "app::Widget") widget_scoped = true;
    }
    check(widget_scoped, "resize scope is app::Widget");

    // ---- Symbol-level call edges (C++) ----
    std::cout << "[symbol call edges: C++]\n";
    std::string calls_cpp = write_fixture(tmp, "calls.cpp", R"cpp(
#include "api.h"

void helper(int x);

int compute(int a, int b) {
    helper(a);
    int r = compute(b, a); // recursion
    return r;
}

class Svc {
public:
    void run() {
        helper(1);
        other();
    }
};
)cpp");
    parser::FileMetadata mdcalls = parser::parse_cpp(calls_cpp);
    check(has_call(mdcalls, "compute", "helper"), "compute -> helper call edge");
    check(has_call(mdcalls, "compute", "compute"), "recursive call edge");
    check(has_call(mdcalls, "Svc::run", "helper"), "method call edge with qualified caller");
    check(has_call(mdcalls, "Svc::run", "other"), "unknown callee still captured");

    // ---- C ----
    std::cout << "[cpp_parser: C]\n";
    std::string c = write_fixture(tmp, "matrix.c", R"c(
#include <stdlib.h>
#include "matrix.h"

typedef struct Matrix {
    int rows;
    int cols;
    double* data;
} Matrix;

Matrix* matrix_create(int rows, int cols) {
    Matrix* m = malloc(sizeof(Matrix));
    m->rows = rows;
    m->cols = cols;
    return m;
}

int matrix_trace(const Matrix* m) {
    int t = 0;
    return t;
}
)c");

    parser::FileMetadata mdc = parser::parse_cpp(c);
    check(mdc.language == "c", "language is c");
    check(has_symbol(mdc, "Matrix", "struct"), "struct Matrix found");
    check(has_symbol(mdc, "matrix_create", "function"), "matrix_create found");
    check(has_symbol(mdc, "matrix_trace", "function"), "matrix_trace found");
    check(has_dep(mdc, "matrix.h"), "include matrix.h found");
    check(has_dep(mdc, "stdlib.h"), "include <stdlib.h> found");

    // ---- Python ----
    std::cout << "[python_parser]\n";
    std::string py = write_fixture(tmp, "service.py", R"py(
import os
from typing import List
from .utils import helper

@dataclass
class Config:
    name: str = "default"
    retries: int = 3

    def validate(self) -> bool:
        return bool(self.name)

async def fetch(url: str) -> str:
    return url

def main(argv: List[str]) -> int:
    cfg = Config()
    return 0 if cfg.validate() else 1
)py");

    parser::FileMetadata mdp = parser::parse_python(py);
    check(mdp.language == "python", "language is python");
    check(has_symbol(mdp, "Config", "class"), "class Config found");
    check(has_symbol(mdp, "validate", "function"), "method validate found");
    check(has_symbol(mdp, "fetch", "async_function"), "async def fetch found");
    check(has_symbol(mdp, "main", "function"), "def main found");
    check(has_dep(mdp, "os"), "import os found");
    check(has_dep(mdp, "typing"), "from typing import found");
    check(has_dep(mdp, "utils"), "from .utils import found");

    // Class scope
    bool validate_scoped = false;
    for (const auto& s : mdp.symbols) {
        if (s.name == "validate" && s.scope == "Config") validate_scoped = true;
    }
    check(validate_scoped, "validate scope is Config");

    // ---- Symbol-level call edges (Python) ----
    std::cout << "[symbol call edges: Python]\n";
    check(has_call(mdp, "main", "Config"), "main -> Config() constructor call");
    check(has_call(mdp, "Config.validate", "bool"), "method call edge with qualified caller");

    // ---- CUDA ----
    std::cout << "[cuda_parser]\n";
    std::string cu = write_fixture(tmp, "kernel.cu", R"cu(
#include <cuda_runtime.h>
#include "kernels.h"

__global__ void scale_kernel(float* data, float factor, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) data[i] *= factor;
}

__device__ float helper(float x) {
    return x * x;
}

void launch(float* d, int n) {
    scale_kernel<<<(n + 255) / 256, 256>>>(d, 2.0f, n);
}
)cu");

    parser::FileMetadata mdu = parser::parse_cuda(cu);
    check(mdu.language == "cuda", "language is cuda");
    check(mdu.file_type == "cuda", "file_type is cuda");
    check(has_symbol(mdu, "scale_kernel", "kernel"), "__global__ kernel found");
    check(has_symbol(mdu, "helper", "device_function"), "__device__ function found");
    check(has_symbol(mdu, "launch", "function"), "host launch function found");
    check(has_dep(mdu, "cuda_runtime.h"), "include cuda_runtime.h found");
    check(has_dep(mdu, "kernels.h"), "include kernels.h found");

    // ---- Graceful degradation ----
    std::cout << "[graceful degradation]\n";
    parser::FileMetadata missing = parser::parse_file((tmp / "does_not_exist.cpp").string(), "cpp");
    check(missing.symbols.empty(), "missing file returns empty metadata (no crash)");

    std::string broken = write_fixture(tmp, "broken.cpp", "class {{{ broken !! (( ");
    parser::FileMetadata mdb = parser::parse_file(broken, "cpp");
    check(mdb.line_count >= 1, "broken file still parsed without crash");

    std::string other = write_fixture(tmp, "notes.txt", "just some text\n");
    parser::FileMetadata mdt = parser::parse_file(other, "unknown");
    check(mdt.file_type == "other", "unsupported language returns basic metadata");

    // ---- End-to-end: indexer -> knowledge store -> queries ----
    std::cout << "[indexer end-to-end]\n";
    {
        knowledge_store::KnowledgeStore store;
        std::string db = (tmp / "test.db").string();
        check(store.open(db), "knowledge store opens");

        indexer::index_file(store, cpp, "repo-1");
        indexer::index_file(store, py, "repo-1");

        check(!indexer::needs_indexing(store, cpp, "repo-1"), "indexed file not stale");

        auto syms = symbol_index::get_file_symbols(store, "repo-1", cpp);
        check(!syms.empty(), "symbols queryable after indexing");

        auto deps = dependency_graph::get_dependencies(store, "repo-1", cpp);
        bool found_widget_h = false;
        for (const auto& d : deps) if (d == "widget.h") found_widget_h = true;
        check(found_widget_h, "dependency queryable after indexing");

        summary_store::store_file_summary(store, "repo-1", cpp, "Widget module");
        check(summary_store::get_file_summary(store, "repo-1", cpp) == "Widget module", "summary stored and retrieved");

        // Modify the file -> hash changes -> needs re-indexing
        {
            std::ofstream f(cpp);
            f << "// changed\n" << R"cpp(
class Widget {
public:
    void draw() {}
};
)cpp";
        }
        check(indexer::needs_indexing(store, cpp, "repo-1"), "modified file detected as stale");
        indexer::index_file(store, cpp, "repo-1");
        check(!indexer::needs_indexing(store, cpp, "repo-1"), "re-indexed file fresh again");
    }

    fs::remove_all(tmp);

    std::cout << (failures == 0 ? "\n[parser tests] ALL PASSED\n" : "\n[parser tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
