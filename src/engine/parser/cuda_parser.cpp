#include "cuda_parser.h"
#include "../parser.h"

#include <tree_sitter/api.h>

#include <cstring>
#include <string>

// Grammar entry point (CUDA is parsed with the C++ grammar; CUDA-specific
// attributes like __global__ appear as attribute_declaration children)
extern "C" {
extern const TSLanguage* tree_sitter_cpp(void);
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

// Detect CUDA qualifiers (__global__, __device__, __host__, __shared__) in the
// text preceding the function declarator.
std::string cuda_qualifier(const std::string& prefix) {
    if (prefix.find("__global__") != std::string::npos) return "kernel";
    if (prefix.find("__device__") != std::string::npos) return "device_function";
    if (prefix.find("__host__") != std::string::npos) return "host_function";
    if (prefix.find("__shared__") != std::string::npos) return "shared_variable";
    return "";
}

class CudaSymbolVisitor {
public:
    CudaSymbolVisitor(const std::string& source, FileMetadata& metadata)
        : source_(source), metadata_(metadata) {}

    void visit(TSNode node, const std::string& scope) {
        std::string type = ts_node_type(node);

        if (type == "namespace_definition") {
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

        if (type == "class_specifier" || type == "struct_specifier") {
            TSNode name = child_by_field(node, "name");
            if (node_is_named(name)) {
                SymbolInfo sym;
                sym.name = node_text(name, source_);
                sym.symbol_type = (type == "class_specifier") ? "class" : "struct";
                sym.signature = normalize(node_text(node, source_));
                sym.start_line = ts_node_start_point(node).row + 1;
                sym.end_line = ts_node_end_point(node).row + 1;
                sym.scope = scope;
                sym.is_definition = node_is_named(child_by_field(node, "body"));
                metadata_.symbols.push_back(sym);

                TSNode body = child_by_field(node, "body");
                if (node_is_named(body)) {
                    std::string child_scope = scope.empty() ? sym.name : scope + "::" + sym.name;
                    uint32_t count = ts_node_named_child_count(body);
                    for (uint32_t i = 0; i < count; ++i) {
                        visit(ts_node_named_child(body, i), child_scope);
                    }
                }
            }
            return;
        }

        if (type == "function_definition") {
            emit_function(node, scope);
            return;
        }

        if (type == "declaration") {
            // CUDA __global__ kernels sometimes parse as a declaration with an
            // attribute; classify by prefix text
            std::string text = normalize(node_text(node, source_));
            std::string qualifier = cuda_qualifier(text);
            if (!qualifier.empty() && qualifier != "shared_variable" && text.find('(') != std::string::npos) {
                SymbolInfo sym;
                sym.symbol_type = qualifier;
                sym.signature = text;
                // name: identifier before '('
                size_t paren = text.find('(');
                size_t end = paren;
                while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '*')) end--;
                size_t begin = end;
                while (begin > 0 && (isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_')) begin--;
                sym.name = text.substr(begin, end - begin);
                sym.start_line = ts_node_start_point(node).row + 1;
                sym.end_line = ts_node_end_point(node).row + 1;
                sym.scope = scope;
                sym.is_definition = true;
                if (!sym.name.empty()) metadata_.symbols.push_back(sym);
                return;
            }
            return;
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i), scope);
        }
    }

private:
    void emit_function(TSNode node, const std::string& scope) {
        TSNode declarator = child_by_field(node, "declarator");
        if (!node_is_named(declarator)) return;

        TSNode cur = declarator;
        while (node_is_named(cur)) {
            std::string t = ts_node_type(cur);
            if (t == "identifier" || t == "field_identifier" || t == "operator_name" ||
                t == "destructor_name" || t == "qualified_identifier") {
                break;
            }
            TSNode next = child_by_field(cur, "declarator");
            if (!node_is_named(next)) break;
            cur = next;
        }

        SymbolInfo sym;
        sym.name = node_text(cur, source_);
        if (sym.name.empty()) return;

        // Inspect the text before the declarator for CUDA qualifiers
        TSNode body = child_by_field(node, "body");
        uint32_t start = ts_node_start_byte(node);
        uint32_t decl_start = ts_node_start_byte(declarator);
        std::string prefix = source_.substr(start, decl_start - start);
        std::string qualifier = cuda_qualifier(prefix);

        sym.symbol_type = qualifier.empty() ? "function" : qualifier;
        uint32_t end = node_is_named(body) ? ts_node_start_byte(body) : ts_node_end_byte(node);
        sym.signature = normalize(source_.substr(start, end - start));
        sym.start_line = ts_node_start_point(node).row + 1;
        sym.end_line = ts_node_end_point(node).row + 1;
        sym.scope = scope;
        sym.is_definition = true;
        metadata_.symbols.push_back(sym);

        // Symbol-level dependency edges: calls made inside this definition
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
            std::string callee;
            if (node_is_named(fn)) {
                std::string ft = ts_node_type(fn);
                if (ft == "identifier" || ft == "qualified_identifier" || ft == "field_identifier") {
                    callee = node_text(fn, source_);
                } else if (ft == "field_expression") {
                    TSNode attr = child_by_field(fn, "field");
                    if (node_is_named(attr)) callee = node_text(attr, source_);
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

FileMetadata parse_cuda(const std::string& file_path) {
    FileMetadata metadata;

    std::string content;
    if (!detail::read_file_content(file_path, content)) {
        metadata.path = file_path;
        metadata.language = "cuda";
        return metadata;
    }
    detail::fill_basic_metadata(metadata, file_path, content);
    metadata.language = "cuda";

    if (metadata.is_binary) {
        return metadata;
    }

    TsParser parser;
    if (!ts_parser_set_language(parser.p, tree_sitter_cpp())) {
        return metadata; // ABI mismatch — degrade gracefully
    }

    TsTree tree(ts_parser_parse_string(parser.p, nullptr, content.c_str(), static_cast<uint32_t>(content.size())));
    if (!tree.t) {
        return metadata; // parsing failure is isolated
    }

    TSNode root = ts_tree_root_node(tree.t);

    CudaSymbolVisitor visitor(content, metadata);
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        visitor.visit(ts_node_named_child(root, i), "");
    }

    extract_includes(root, content, metadata);

    return metadata;
}

}
