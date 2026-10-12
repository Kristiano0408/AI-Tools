#include "python_parser.h"
#include "../parser.h"

#include <tree_sitter/api.h>

#include <cstring>
#include <string>

// Grammar entry point (provided by tree-sitter-python)
extern "C" {
extern const TSLanguage* tree_sitter_python(void);
}

namespace parser {

namespace {

struct TsParser {
    TSParser* p = nullptr;
    TsParser() : p(ts_parser_new()) {}
    ~TsParser() { if (p) ts_parser_delete(p); }
    TsParser(const TsParser&) = delete;
    TsParser& operator=(const TsParser&) = delete;
};

struct TsTree {
    TSTree* t = nullptr;
    TsTree() = default;
    explicit TsTree(TSTree* tree) : t(tree) {}
    ~TsTree() { if (t) ts_tree_delete(t); }
    TsTree(const TsTree&) = delete;
    TsTree& operator=(const TsTree&) = delete;
};

std::string node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end <= start || end > source.size()) return "";
    return source.substr(start, end - start);
}

std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!out.empty() && !in_space) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

TSNode child_by_field(TSNode node, const char* field) {
    return ts_node_child_by_field_name(node, field, static_cast<uint32_t>(strlen(field)));
}

bool node_is_named(TSNode node) {
    if (ts_node_is_null(node)) return false;
    return ts_node_is_named(node);
}

// ---- Python symbol extraction ----

class PythonSymbolVisitor {
public:
    PythonSymbolVisitor(const std::string& source, FileMetadata& metadata)
        : source_(source), metadata_(metadata) {}

    void visit(TSNode node, const std::string& scope) {
        std::string type = ts_node_type(node);

        if (type == "class_definition") {
            TSNode name = child_by_field(node, "name");
            SymbolInfo sym;
            sym.name = node_is_named(name) ? node_text(name, source_) : "";
            sym.symbol_type = "class";
            sym.signature = normalize(class_signature(node));
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            metadata_.symbols.push_back(sym);

            // Recurse into the class body with the class as scope
            TSNode body = child_by_field(node, "body");
            if (node_is_named(body)) {
                std::string child_scope = scope.empty() ? sym.name : scope + "." + sym.name;
                uint32_t count = ts_node_named_child_count(body);
                for (uint32_t i = 0; i < count; ++i) {
                    visit(ts_node_named_child(body, i), child_scope);
                }
            }
            return;
        }

        if (type == "function_definition") {
            TSNode name = child_by_field(node, "name");
            SymbolInfo sym;
            sym.name = node_is_named(name) ? node_text(name, source_) : "";
            // async def is decorated by an "async" keyword in the decorated_definition;
            // tree-sitter-python marks it via the parent decorated_definition or the
            // function_definition itself starts with "async"
            std::string sig = normalize(function_signature(node));
            sym.symbol_type = (sig.rfind("async ", 0) == 0) ? "async_function" : "function";
            sym.signature = sig;
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            metadata_.symbols.push_back(sym);

            // Symbol-level dependency edges: calls made inside this function.
            // Nested defs are attributed to the inner function instead.
            TSNode body = child_by_field(node, "body");
            std::string child_scope = scope.empty() ? sym.name : scope + "." + sym.name;
            if (node_is_named(body)) {
                collect_calls(body, child_scope);

                // Recurse into the function body (nested defs)
                uint32_t count = ts_node_named_child_count(body);
                for (uint32_t i = 0; i < count; ++i) {
                    visit(ts_node_named_child(body, i), child_scope);
                }
            }
            return;
        }

        if (type == "decorated_definition") {
            TSNode def = child_by_field(node, "definition");
            if (node_is_named(def)) {
                visit(def, scope);
            }
            return;
        }

        if (type == "decorator") {
            SymbolInfo sym;
            sym.name = normalize(node_text(node, source_)); // includes '@'
            sym.symbol_type = "decorator";
            sym.signature = sym.name;
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_declaration = true;
            metadata_.symbols.push_back(sym);
            return;
        }

        if (type == "expression_statement") {
            // Module-level (or class-level) assignment: "x = ..." / "x: T = ..."
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_named_child(node, i);
                std::string ct = ts_node_type(child);
                if (ct == "assignment" || ct == "augmented_assignment" || ct == "typed_parameter" ||
                    ct == "annotated_variable_declarator" || ct == "variable_assignment") {
                    emit_assignment(child, scope);
                }
            }
            return;
        }

        // Default: recurse into named children
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i), scope);
        }
    }

private:
    std::string class_signature(TSNode node) {
        TSNode body = child_by_field(node, "body");
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = node_is_named(body) ? ts_node_start_byte(body) : ts_node_end_byte(node);
        return source_.substr(start, end - start);
    }

    std::string function_signature(TSNode node) {
        TSNode body = child_by_field(node, "body");
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = node_is_named(body) ? ts_node_start_byte(body) : ts_node_end_byte(node);
        return source_.substr(start, end - start);
    }

    void emit_assignment(TSNode node, const std::string& scope) {
        TSNode left = child_by_field(node, "left");
        if (!node_is_named(left)) return;
        if (std::string(ts_node_type(left)) != "identifier") return; // skip tuple/attribute targets

        SymbolInfo sym;
        sym.name = node_text(left, source_);
        sym.symbol_type = "variable";
        sym.signature = normalize(node_text(node, source_));
        sym.start_line = ts_node_start_point(node).row + 1;
        sym.end_line = ts_node_end_point(node).row + 1;
        sym.scope = scope;
        sym.is_definition = true;
        metadata_.symbols.push_back(sym);
    }

    void collect_calls(TSNode node, const std::string& caller) {
        std::string t = ts_node_type(node);
        if (t == "function_definition" || t == "class_definition") return; // inner scopes own their calls

        if (t == "call") {
            TSNode fn = child_by_field(node, "function");
            std::string callee;
            if (node_is_named(fn)) {
                std::string ft = ts_node_type(fn);
                if (ft == "identifier" || ft == "attribute") {
                    callee = node_text(fn, source_);
                    // obj.method(...) -> keep just the method name for matching
                    size_t dot = callee.rfind('.');
                    if (dot != std::string::npos) callee = callee.substr(dot + 1);
                }
            }
            if (!callee.empty()) {
                SymbolDependency dep;
                dep.caller = caller;
                dep.callee = callee;
                dep.line = ts_node_start_point(node).row + 1;
                metadata_.symbol_deps.push_back(dep);
            }
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            collect_calls(ts_node_named_child(node, i), caller);
        }
    }

    const std::string& source_;
    FileMetadata& metadata_;
};

// ---- dependency extraction ----

void extract_imports(TSNode node, const std::string& source, FileMetadata& metadata) {
    std::string t = ts_node_type(node);
    if (t == "import_statement") {
        // "import a, b.c" -> dotted_name children
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (std::string(ts_node_type(child)) == "dotted_name" ||
                std::string(ts_node_type(child)) == "aliased_import") {
                TSNode target = child;
                if (std::string(ts_node_type(child)) == "aliased_import") {
                    target = child_by_field(child, "name");
                }
                if (node_is_named(target)) {
                    metadata.dependencies.push_back(node_text(target, source));
                }
            }
        }
        return;
    }
    if (t == "import_from_statement") {
        TSNode module = child_by_field(node, "module_name");
        if (node_is_named(module)) {
            std::string dep = node_text(module, source);
            // Normalize relative imports: ".utils" / "..pkg" -> "utils" / "pkg"
            size_t i = 0;
            while (i < dep.size() && dep[i] == '.') i++;
            dep = dep.substr(i);
            if (!dep.empty()) metadata.dependencies.push_back(dep);
        }
        return;
    }
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_imports(ts_node_named_child(node, i), source, metadata);
    }
}

} // anonymous namespace

FileMetadata parse_python(const std::string& file_path) {
    FileMetadata metadata;

    std::string content;
    if (!detail::read_file_content(file_path, content)) {
        metadata.path = file_path;
        metadata.language = "python";
        return metadata;
    }
    detail::fill_basic_metadata(metadata, file_path, content);
    metadata.language = "python";

    if (metadata.is_binary) {
        return metadata;
    }

    TsParser parser;
    if (!ts_parser_set_language(parser.p, tree_sitter_python())) {
        return metadata; // ABI mismatch — degrade gracefully
    }

    TsTree tree(ts_parser_parse_string(parser.p, nullptr, content.c_str(), static_cast<uint32_t>(content.size())));
    if (!tree.t) {
        return metadata; // parsing failure is isolated
    }

    TSNode root = ts_tree_root_node(tree.t);

    // Symbols
    PythonSymbolVisitor visitor(content, metadata);
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        visitor.visit(ts_node_named_child(root, i), "");
    }

    // Dependencies (import / from ... import)
    extract_imports(root, content, metadata);

    return metadata;
}

}
