#include "cpp_parser.h"
#include "../parser.h"

#include <tree_sitter/api.h>

#include <cctype>
#include <cstring>
#include <string>

// Grammar entry points (provided by tree-sitter-c / tree-sitter-cpp)
extern "C" {
extern const TSLanguage* tree_sitter_c(void);
extern const TSLanguage* tree_sitter_cpp(void);
}

namespace parser {

namespace {

// ---- small RAII wrappers around the tree-sitter C API ----

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

// ---- helpers ----

std::string node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end <= start || end > source.size()) return "";
    return source.substr(start, end - start);
}

// Collapse whitespace/newlines into a single line (signatures are often multi-line)
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
    // ts_node_child_by_field_name returns a null node when the field is absent;
    // calling ts_node_is_named on it dereferences a null child pointer.
    if (ts_node_is_null(node)) return false;
    return ts_node_is_named(node);
}

// ---- C/C++ symbol extraction ----

class CppSymbolVisitor {
public:
    CppSymbolVisitor(const std::string& source, FileMetadata& metadata, bool is_cpp)
        : source_(source), metadata_(metadata), is_cpp_(is_cpp) {}

    void visit(TSNode node, const std::string& scope) {
        std::string type = ts_node_type(node);

        if (is_cpp_ && type == "namespace_definition") {
            TSNode name = child_by_field(node, "name");
            std::string ns_name = node_is_named(name) ? node_text(name, source_) : "";
            SymbolInfo sym;
            sym.name = ns_name;
            sym.symbol_type = "namespace";
            sym.signature = "namespace " + ns_name;
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            metadata_.symbols.push_back(sym);

            std::string child_scope = scope.empty() ? ns_name : scope + "::" + ns_name;
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                visit(ts_node_named_child(node, i), child_scope);
            }
            return;
        }

        if (type == "class_specifier" || type == "struct_specifier" || type == "union_specifier") {
            emit_type(node, type, scope);
            // Recurse into the body to collect member functions/fields
            TSNode body = child_by_field(node, "body");
            if (node_is_named(body)) {
                std::string child_scope = qualified(scope, node);
                uint32_t count = ts_node_named_child_count(body);
                for (uint32_t i = 0; i < count; ++i) {
                    visit(ts_node_named_child(body, i), child_scope);
                }
            }
            return;
        }

        if (type == "enum_specifier") {
            SymbolInfo sym;
            TSNode name = child_by_field(node, "name");
            sym.name = node_is_named(name) ? node_text(name, source_) : "";
            sym.symbol_type = "enum";
            sym.signature = normalize(node_text(node, source_));
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            metadata_.symbols.push_back(sym);
            return;
        }

        if (type == "function_definition") {
            emit_function(node, scope);
            return;
        }

        if (type == "declaration") {
            emit_declaration(node, scope);
            return;
        }

        if (is_cpp_ && (type == "field_declaration" || type == "function_declaration" ||
                        type == "prototype_function_declaration")) {
            emit_declaration(node, scope);
            return;
        }

        if (is_cpp_ && type == "template_declaration") {
            // Descend into the templated declaration
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                visit(ts_node_named_child(node, i), scope);
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
    std::string qualified(const std::string& scope, TSNode type_node) {
        TSNode name = child_by_field(type_node, "name");
        if (!node_is_named(name)) return scope;
        std::string n = node_text(name, source_);
        return scope.empty() ? n : scope + "::" + n;
    }

    void emit_type(TSNode node, const std::string& ts_type, const std::string& scope) {
        TSNode name = child_by_field(node, "name");
        if (!node_is_named(name)) return; // anonymous struct/union
        SymbolInfo sym;
        sym.name = node_text(name, source_);
        sym.symbol_type = (ts_type == "class_specifier") ? "class" : (ts_type == "union_specifier") ? "union" : "struct";
        sym.signature = normalize(node_text(node, source_));
        sym.start_line = ts_node_start_point(node).row + 1;
        sym.end_line = ts_node_end_point(node).row + 1;
        sym.scope = scope;
        sym.is_definition = node_is_named(child_by_field(node, "body"));
        metadata_.symbols.push_back(sym);
    }

    void emit_function(TSNode node, const std::string& scope) {
        TSNode declarator = child_by_field(node, "declarator");
        if (!node_is_named(declarator)) return;

        // Walk through pointer/reference/function declarators down to the identifier
        TSNode cur = declarator;
        while (node_is_named(cur)) {
            std::string t = ts_node_type(cur);
            if (t == "identifier" || t == "field_identifier" || t == "operator_name" ||
                t == "destructor_name" || t == "qualified_identifier" || t == "template_function_id") {
                break;
            }
            TSNode next = child_by_field(cur, "declarator");
            if (!node_is_named(next)) break;
            cur = next;
        }

        SymbolInfo sym;
        sym.name = node_text(cur, source_);
        if (sym.name.empty()) return;
        sym.symbol_type = "function";
        sym.signature = normalize(function_signature(node));
        sym.start_line = ts_node_start_point(node).row + 1;
        sym.end_line = ts_node_end_point(node).row + 1;
        sym.scope = scope;
        sym.is_definition = true;
        metadata_.symbols.push_back(sym);

        // Symbol-level dependency edges: calls made inside this definition.
        // Nested definitions are attributed to the inner function instead.
        TSNode body = child_by_field(node, "body");
        if (node_is_named(body)) {
            std::string caller = scope.empty() ? sym.name : scope + "::" + sym.name;
            collect_calls(body, caller);
        }
    }

    void collect_calls(TSNode node, const std::string& caller) {
        std::string t = ts_node_type(node);

        if (t == "function_definition") return; // inner function owns its calls

        if (t == "call_expression") {
            TSNode fn = child_by_field(node, "function");
            std::string callee = callee_name(fn);
            if (!callee.empty()) {
                SymbolDependency dep;
                dep.caller = caller;
                dep.callee = callee;
                dep.line = ts_node_start_point(node).row + 1;
                metadata_.symbol_deps.push_back(dep);
            }
            // fall through to also capture nested calls in the arguments
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            collect_calls(ts_node_named_child(node, i), caller);
        }
    }

    std::string callee_name(TSNode fn) {
        if (!node_is_named(fn)) return "";
        std::string ft = ts_node_type(fn);
        if (ft == "identifier" || ft == "qualified_identifier" || ft == "field_identifier" ||
            ft == "destructor_name" || ft == "operator_name" || ft == "template_function_id") {
            return node_text(fn, source_);
        }
        if (ft == "field_expression") { // obj.method(...) / obj->method(...)
            TSNode attr = child_by_field(fn, "field");
            if (node_is_named(attr)) return node_text(attr, source_);
        }
        return "";
    }

    std::string function_signature(TSNode func_node) {
        // Return type + declarator up to (but excluding) the body
        TSNode body = child_by_field(func_node, "body");
        uint32_t start = ts_node_start_byte(func_node);
        uint32_t end = node_is_named(body) ? ts_node_start_byte(body) : ts_node_end_byte(func_node);
        return source_.substr(start, end - start);
    }

    void emit_declaration(TSNode node, const std::string& scope) {
        std::string text = normalize(node_text(node, source_));
        if (text.empty()) return;

        // Function prototype declaration: contains '(' and ends with ';'
        bool is_function_decl = text.find('(') != std::string::npos && text.back() == ';';
        if (is_function_decl) {
            size_t paren = text.find('(');
            size_t end = paren;
            while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '*')) end--;
            size_t begin = end;
            while (begin > 0 && (isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_')) begin--;
            if (begin < end) {
                SymbolInfo sym;
                sym.name = text.substr(begin, end - begin);
                sym.symbol_type = "function";
                sym.signature = text;
                sym.start_line = ts_node_start_point(node).row + 1;
                sym.end_line = ts_node_end_point(node).row + 1;
                sym.scope = scope;
                sym.is_declaration = true;
                metadata_.symbols.push_back(sym);
            }
            return;
        }

        // typedef / using declaration
        bool is_typedef = text.rfind("typedef ", 0) == 0;
        bool is_using = text.rfind("using ", 0) == 0;
        if (is_typedef || is_using) {
            SymbolInfo sym;
            sym.symbol_type = is_typedef ? "typedef" : "using";
            sym.signature = text;
            // Best-effort name: last identifier before '=' or ';'
            size_t cut = text.find('=');
            if (cut == std::string::npos) cut = text.find(';');
            if (cut != std::string::npos) {
                size_t end = cut;
                while (end > 0 && text[end - 1] == ' ') end--;
                size_t begin = end;
                while (begin > 0 && (isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_')) begin--;
                sym.name = text.substr(begin, end - begin);
            }
            sym.start_line = ts_node_start_point(node).row + 1;
            sym.end_line = ts_node_end_point(node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            if (!sym.name.empty()) metadata_.symbols.push_back(sym);
            return;
        }

        // Plain variable declaration: collect declarator identifiers
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            std::string ct = ts_node_type(child);
            if (ct == "init_declarator" || ct == "declarator") {
                collect_identifiers(child, text, scope, node);
            }
        }
    }

    void collect_identifiers(TSNode node, const std::string& signature, const std::string& scope, TSNode decl_node) {
        std::string t = ts_node_type(node);
        if (t == "identifier") {
            SymbolInfo sym;
            sym.name = node_text(node, source_);
            sym.symbol_type = "variable";
            sym.signature = signature;
            sym.start_line = ts_node_start_point(decl_node).row + 1;
            sym.end_line = ts_node_end_point(decl_node).row + 1;
            sym.scope = scope;
            sym.is_definition = true;
            metadata_.symbols.push_back(sym);
            return;
        }
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            collect_identifiers(ts_node_named_child(node, i), signature, scope, decl_node);
        }
    }

    const std::string& source_;
    FileMetadata& metadata_;
    bool is_cpp_;
};

// ---- dependency extraction ----

void extract_includes(TSNode node, const std::string& source, FileMetadata& metadata) {
    std::string t = ts_node_type(node);
    if (t == "preproc_include") {
        TSNode path = child_by_field(node, "path");
        if (node_is_named(path)) {
            std::string dep = node_text(path, source);
            if (!dep.empty() && (dep.front() == '"' || dep.front() == '<')) dep.erase(dep.begin());
            if (!dep.empty() && (dep.back() == '"' || dep.back() == '>')) dep.pop_back();
            if (!dep.empty()) metadata.dependencies.push_back(dep);
        }
        return;
    }
    uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_includes(ts_node_named_child(node, i), source, metadata);
    }
}

} // anonymous namespace

FileMetadata parse_cpp(const std::string& file_path) {
    FileMetadata metadata;

    std::string content;
    if (!detail::read_file_content(file_path, content)) {
        metadata.path = file_path;
        metadata.language = "cpp";
        return metadata;
    }
    detail::fill_basic_metadata(metadata, file_path, content);

    // tree-sitter-c for .c files, tree-sitter-cpp for C++ files
    std::string ext = file_path.substr(file_path.rfind('.') + 1);
    bool is_c = (ext == "c");
    metadata.language = is_c ? "c" : "cpp";

    if (metadata.is_binary) {
        return metadata;
    }

    const TSLanguage* language = is_c ? tree_sitter_c() : tree_sitter_cpp();

    TsParser parser;
    if (!ts_parser_set_language(parser.p, language)) {
        return metadata; // ABI mismatch — degrade gracefully
    }

    TsTree tree(ts_parser_parse_string(parser.p, nullptr, content.c_str(), static_cast<uint32_t>(content.size())));
    if (!tree.t) {
        return metadata; // parsing failure is isolated
    }

    TSNode root = ts_tree_root_node(tree.t);

    // Symbols
    CppSymbolVisitor visitor(content, metadata, !is_c);
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        visitor.visit(ts_node_named_child(root, i), "");
    }

    // Dependencies (#include)
    extract_includes(root, content, metadata);

    return metadata;
}

}
