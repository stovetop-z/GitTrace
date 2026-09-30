#ifndef GITTRACE_TREESITTER_H
#define GITTRACE_TREESITTER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <cstddef>

struct TSLanguage;
struct TSParser;

class TreeSitter
{
public:
    explicit TreeSitter(std::string resource_dir);
    ~TreeSitter();

    TreeSitter(const TreeSitter&) = delete;
    TreeSitter& operator=(const TreeSitter&) = delete;

    // Install and load grammars for the supplied file extensions. Returns false
    // if a listed grammar cannot be cloned, built, or loaded.
    bool prepare(const std::vector<std::string>& extensions);

    // Parse one source buffer with the grammar selected by its file extension.
    // Returns false for unsupported extensions and parser errors.
    bool parse(const std::string& filepath, const char* source, size_t length);

private:
    struct Grammar
    {
        void* library = nullptr;
        const TSLanguage* language = nullptr;
        TSParser* parser = nullptr;
    };
    struct GrammarSpec
    {
        std::string url;
        std::string source_subdir;
        std::string symbol;
        std::string key;
    };
    std::string resource_dir_;
    std::unordered_map<std::string, Grammar> grammars_;
    std::unordered_map<std::string, GrammarSpec> extension_to_grammar_;

    bool load_manifest();
    bool install_and_load(const GrammarSpec& spec);
};

#endif
