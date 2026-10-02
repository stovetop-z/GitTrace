#include "treesitter.h"

#include <tree_sitter/api.h>
#include <git2.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace
{
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string extension_of(const std::string& filepath)
{
    const std::string filename = fs::path(filepath).filename().string();
    const auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= filename.size()) return {};
    return lowercase(filename.substr(dot + 1));
}

std::string repo_name(const std::string& url)
{
    std::string name = url.substr(url.find_last_of('/') + 1);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".git") name.resize(name.size() - 4);
    return name;
}

std::string language_symbol(const std::string& repository_name)
{
    std::string name = repository_name;
    const std::string prefix = "tree-sitter-";
    if (name.compare(0, prefix.size(), prefix) == 0) name.erase(0, prefix.size());
    std::replace(name.begin(), name.end(), '-', '_');
    return "tree_sitter_" + name;
}

std::string shell_quote(const std::string& value)
{
#ifdef _WIN32
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"') quoted += "\\\"";
        else quoted += c;
    }
    return quoted + "\"";
#else
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
#endif
}

void* open_library(const fs::path& path)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryW(path.wstring().c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* library, const std::string& symbol)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), symbol.c_str()));
#else
    return dlsym(library, symbol.c_str());
#endif
}

void close_library(void* library)
{
    if (!library) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

std::string dynamic_library_name()
{
#ifdef _WIN32
    return "gittrace_grammar.dll";
#elif __APPLE__
    return "gittrace_grammar.so";
#else
    return "gittrace_grammar.so";
#endif
}
}

TreeSitter::TreeSitter(std::string resource_dir) : resource_dir_(std::move(resource_dir))
{
}

TreeSitter::~TreeSitter()
{
    for (auto& entry : grammars_) {
        if (entry.second.parser) ts_parser_delete(entry.second.parser);
        close_library(entry.second.library);
    }
}

bool TreeSitter::load_manifest()
{
    const fs::path manifest = fs::path(resource_dir_) / "grammar.txt";
    std::ifstream input(manifest);
    if (!input) {
        std::cerr << "Could not open grammar manifest: " << manifest << '\n';
        return false;
    }

    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            std::cerr << "Ignoring malformed grammar manifest line " << line_number << '\n';
            continue;
        }
        const std::string extension = lowercase(trim(line.substr(0, equals)));
        std::istringstream fields(line.substr(equals + 1));
        std::vector<std::string> values;
        std::string field;
        while (std::getline(fields, field, '|')) values.push_back(trim(field));
        const std::string url = values.empty() ? std::string() : values[0];
        if (extension.empty() || (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)) {
            std::cerr << "Ignoring invalid grammar manifest entry on line " << line_number << '\n';
            continue;
        }
        GrammarSpec spec;
        spec.url = url;
        spec.source_subdir = values.size() > 1 && !values[1].empty() ? values[1] : "";
        spec.symbol = values.size() > 2 ? values[2] : "";
        spec.key = url + "|" + spec.source_subdir + "|" + spec.symbol;
        extension_to_grammar_[extension] = std::move(spec);
    }
    return true;
}

bool TreeSitter::prepare(const std::vector<std::string>& extensions)
{
    if (extension_to_grammar_.empty() && !load_manifest()) return false;

    std::map<std::string, GrammarSpec> needed_grammars;
    for (const auto& extension : extensions) {
        const auto found = extension_to_grammar_.find(lowercase(extension));
        if (found == extension_to_grammar_.end()) {
            std::cerr << "No Tree-sitter grammar configured for extension: " << extension << '\n';
            continue;
        }
        needed_grammars.emplace(found->second.key, found->second);
    }

    bool success = true;
    for (const auto& item : needed_grammars) {
        if (!install_and_load(item.second)) success = false;
    }
    return success;
}

bool TreeSitter::install_and_load(const GrammarSpec& spec)
{
    if (grammars_.find(spec.key) != grammars_.end()) return true;

    const std::string name = repo_name(spec.url);
    if (name.empty() || name == "." || name == ".." ||
        spec.source_subdir.find("..") != std::string::npos || fs::path(spec.source_subdir).is_absolute()) {
        std::cerr << "Invalid grammar repository name or source subdirectory\n";
        return false;
    }
    const fs::path grammar_root = fs::path(resource_dir_) / "grammars";
    const fs::path source_dir = grammar_root / name;
    const fs::path build_dir = grammar_root / ".build" / name /
        (spec.source_subdir.empty() ? "root" : fs::path(spec.source_subdir));
    std::error_code ec;
    fs::create_directories(grammar_root, ec);
    if (ec) {
        std::cerr << "Could not create grammar directory " << grammar_root << ": " << ec.message() << '\n';
        return false;
    }

    if (!fs::exists(source_dir / ".git")) {
        if (fs::exists(source_dir)) fs::remove_all(source_dir, ec);
        git_repository* cloned = nullptr;
        const int result = git_clone(&cloned, spec.url.c_str(), source_dir.string().c_str(), nullptr);
        if (cloned) git_repository_free(cloned);
        if (result < 0) {
            const git_error* error = git_error_last();
            std::cerr << "Could not clone grammar " << spec.url << ": "
                      << (error ? error->message : "unknown libgit2 error") << '\n';
            return false;
        }
    }

    const fs::path grammar_source_dir = spec.source_subdir.empty()
        ? source_dir : source_dir / spec.source_subdir;
    if (!fs::exists(grammar_source_dir / "src" / "parser.c")) {
        std::cerr << "Grammar source has no src/parser.c: " << grammar_source_dir << '\n';
        return false;
    }

    fs::create_directories(build_dir, ec);
    if (ec) {
        std::cerr << "Could not create grammar build directory: " << ec.message() << '\n';
        return false;
    }

    const fs::path absolute_build_dir = fs::absolute(build_dir);
    const fs::path absolute_grammar_source_dir = fs::absolute(grammar_source_dir);
    const fs::path cmake_file = absolute_build_dir / "CMakeLists.txt";
    {
        std::ofstream cmake(cmake_file, std::ios::trunc);
        cmake << "cmake_minimum_required(VERSION 3.10)\n"
                 "project(GitTraceGrammar LANGUAGES C CXX)\n"
                 "file(GLOB parser_sources \"${GRAMMAR_SOURCE_DIR}/src/parser.c\" \"${GRAMMAR_SOURCE_DIR}/src/scanner.c\" \"${GRAMMAR_SOURCE_DIR}/src/scanner.cc\" \"${GRAMMAR_SOURCE_DIR}/src/scanner.cpp\")\n"
                 "add_library(gittrace_grammar MODULE ${parser_sources})\n"
                 "target_include_directories(gittrace_grammar PRIVATE \"${GRAMMAR_SOURCE_DIR}/src\")\n"
                 "set_target_properties(gittrace_grammar PROPERTIES PREFIX \"\" OUTPUT_NAME \"gittrace_grammar\" LIBRARY_OUTPUT_DIRECTORY \"${OUTPUT_DIR}\" RUNTIME_OUTPUT_DIRECTORY \"${OUTPUT_DIR}\" LIBRARY_OUTPUT_DIRECTORY_RELEASE \"${OUTPUT_DIR}\" RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${OUTPUT_DIR}\" WINDOWS_EXPORT_ALL_SYMBOLS ON)\n";
        if (!cmake) {
            std::cerr << "Could not write generated grammar CMakeLists: " << cmake_file << '\n';
            return false;
        }
    }

    const fs::path output_dir = absolute_build_dir / "artifacts";
    fs::create_directories(output_dir, ec);
    if (ec) return false;
    const fs::path cmake_build_dir = absolute_build_dir / "cmake-build";
    const std::string configure = "cmake -S " + shell_quote(absolute_build_dir.string()) +
        " -B " + shell_quote(cmake_build_dir.string()) +
        " -DGRAMMAR_SOURCE_DIR=" + shell_quote(absolute_grammar_source_dir.string()) +
        " -DOUTPUT_DIR=" + shell_quote(output_dir.string());
    if (std::system(configure.c_str()) != 0) {
        std::cerr << "CMake configuration failed for grammar " << name << '\n';
        return false;
    }
    const std::string build = "cmake --build " + shell_quote(cmake_build_dir.string()) + " --config Release";
    if (std::system(build.c_str()) != 0) {
        std::cerr << "Could not build Tree-sitter grammar " << name << '\n';
        return false;
    }

    const fs::path library_path = output_dir / dynamic_library_name();
    void* library = open_library(library_path);
    if (!library) {
#ifndef _WIN32
        std::cerr << "Could not load grammar library " << library_path << ": " << dlerror() << '\n';
#else
        std::cerr << "Could not load grammar library " << library_path << '\n';
#endif
        return false;
    }

    using LanguageFunction = const TSLanguage* (*)();
    const std::string symbol = spec.symbol.empty() ? language_symbol(name) : spec.symbol;
    auto function = reinterpret_cast<LanguageFunction>(find_symbol(library, symbol));
    if (!function) {
        std::cerr << "Grammar library " << name << " does not export " << symbol << '\n';
        close_library(library);
        return false;
    }

    Grammar grammar;
    grammar.library = library;
    grammar.language = function();
    grammar.parser = ts_parser_new();
    if (!grammar.language || !grammar.parser ||
        ts_language_abi_version(grammar.language) > TREE_SITTER_LANGUAGE_VERSION ||
        ts_language_abi_version(grammar.language) < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION ||
        !ts_parser_set_language(grammar.parser, grammar.language)) {
        std::cerr << "Grammar " << name << " is incompatible with the installed Tree-sitter runtime\n";
        if (grammar.parser) ts_parser_delete(grammar.parser);
        close_library(library);
        return false;
    }
    grammars_.emplace(spec.key, grammar);
    return true;
}

bool TreeSitter::parse(const std::string& filepath, const char* source, size_t length)
{
    if (length > std::numeric_limits<uint32_t>::max()) return false;
    const std::string extension = extension_of(filepath);
    const auto mapping = extension_to_grammar_.find(extension);
    if (mapping == extension_to_grammar_.end()) return false;
    const auto grammar = grammars_.find(mapping->second.key);
    if (grammar == grammars_.end() || !grammar->second.parser) return false;

    TSTree* tree = ts_parser_parse_string(grammar->second.parser, nullptr, source ? source : "",
                                          static_cast<uint32_t>(length));
    if (!tree) return false;
    const bool valid = !ts_node_has_error(ts_tree_root_node(tree));
    ts_tree_delete(tree);
    return valid;
}

std::string TreeSitter::get_query_pattern_for_extension(const std::string& ext) const
{
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp") {
        return "(function_definition) @fn (class_specifier) @cls (struct_specifier) @st";
    }
    if (ext == "c") return "(function_definition) @fn (struct_specifier) @st";
    if (ext == "py") return "(function_definition) @fn (class_definition) @cls";
    if (ext == "js" || ext == "jsx" || ext == "ts" || ext == "tsx") {
        return "(function_declaration) @fn (method_definition) @fn (class_declaration) @cls";
    }
    if (ext == "rs") return "(function_item) @fn (impl_item) @impl";
    if (ext == "go") return "(function_declaration) @fn (method_declaration) @fn";
    return {};
}

std::vector<SyntaxSpan> TreeSitter::extractSpans(const std::string& filepath, const char* source,
                                                 size_t length) const
{
    std::vector<SyntaxSpan> spans;
    if (!source || length == 0 || length > std::numeric_limits<uint32_t>::max()) return spans;

    const std::string ext = extension_of(filepath);
    const auto mapping = extension_to_grammar_.find(ext);
    if (mapping == extension_to_grammar_.end()) return spans;
    const auto grammar = grammars_.find(mapping->second.key);
    if (grammar == grammars_.end() || !grammar->second.parser || !grammar->second.language) return spans;

    TSTree* tree = ts_parser_parse_string(grammar->second.parser, nullptr, source,
                                          static_cast<uint32_t>(length));
    if (!tree) return spans;

    const std::string query_text = get_query_pattern_for_extension(ext);
    if (!query_text.empty()) {
        uint32_t error_offset = 0;
        TSQueryError error_type = TSQueryErrorNone;
        TSQuery* query = ts_query_new(grammar->second.language, query_text.c_str(),
                                      static_cast<uint32_t>(query_text.size()),
                                      &error_offset, &error_type);
        if (query && error_type == TSQueryErrorNone) {
            TSQueryCursor* cursor = ts_query_cursor_new();
            if (cursor) {
                ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
                TSQueryMatch match;
                while (ts_query_cursor_next_match(cursor, &match)) {
                    for (uint16_t i = 0; i < match.capture_count; ++i) {
                        const TSNode node = match.captures[i].node;
                        const uint32_t start = ts_node_start_byte(node);
                        const uint32_t end = ts_node_end_byte(node);
                        if (start >= end || end > length) continue;
                        const TSPoint first = ts_node_start_point(node);
                        const TSPoint last = ts_node_end_point(node);
                        const uint32_t end_line = last.row + (last.column > 0 ? 1 : 0);
                        spans.push_back({start, end, first.row + 1, std::max(first.row + 1, end_line),
                                         ts_node_type(node)});
                    }
                }
                ts_query_cursor_delete(cursor);
            }
            ts_query_delete(query);
        }
    }

    ts_tree_delete(tree);
    std::sort(spans.begin(), spans.end(), [](const SyntaxSpan& a, const SyntaxSpan& b) {
        if (a.start_byte != b.start_byte) return a.start_byte < b.start_byte;
        return a.end_byte > b.end_byte;
    });
    return spans;
}
