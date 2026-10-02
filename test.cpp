#include "GitTrace/gittrace.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool run(const std::string& command)
{
    return std::system(command.c_str()) == 0;
}

static bool writeFile(const fs::path& path, const std::string& contents)
{
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return out.good();
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "gittrace-retrieval-test";
    const fs::path repo = root / "repo";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(repo, ec);
    if (ec) {
        std::cerr << "Could not create test repository: " << ec.message() << '\n';
        return 1;
    }

    const std::string prefix = "git -C \"" + repo.string() + "\" ";
    const std::vector<std::string> setup = {
        prefix + "init -q",
        prefix + "config user.name \"GitTrace Test\"",
        prefix + "config user.email \"gittrace-test@example.invalid\""
    };
    for (const auto& command : setup) {
        if (!run(command)) {
            std::cerr << "Failed to initialize test Git repository\n";
            fs::remove_all(root, ec);
            return 1;
        }
    }

    if (!writeFile(repo / "alpha.txt", "alpha version one\n") ||
        !writeFile(repo / "unchanged.txt", "kept across commits\n") ||
        !run(prefix + "add .") || !run(prefix + "commit -q -m first") ||
        !writeFile(repo / "alpha.txt", "alpha version two\n") ||
        !writeFile(repo / "beta.txt", "new file in second commit\n") ||
        !run(prefix + "add .") || !run(prefix + "commit -q -m second")) {
        std::cerr << "Failed to create test history\n";
        fs::remove_all(root, ec);
        return 1;
    }

    GitTrace trace;
    if (!trace.init(repo.string().c_str(), false)) {
        std::cerr << "GitTrace could not open the test repository\n";
        fs::remove_all(root, ec);
        return 1;
    }
    trace.beginGitTrace();

    std::map<std::string, std::string> expected = {
        {"alpha.txt|alpha version one\n", "first"},
        {"alpha.txt|alpha version two\n", "second"},
        {"beta.txt|new file in second commit\n", "second"},
        {"unchanged.txt|kept across commits\n", "second"}
    };
    if (trace.blobs.size() != expected.size()) {
        std::cerr << "Expected " << expected.size() << " unique text blobs, got "
                  << trace.blobs.size() << '\n';
        fs::remove_all(root, ec);
        return 1;
    }

    for (const Blob& blob : trace.blobs) {
        const std::string key = blob.filepath + "|" +
            std::string(blob.raw_data.begin(), blob.raw_data.end());
        const auto found = expected.find(key);
        if (found == expected.end()) {
            std::cerr << "Unexpected or corrupted blob retrieved: " << blob.filepath << '\n';
            fs::remove_all(root, ec);
            return 1;
        }
        const std::string revision_command = prefix + "log -1 --format=%s " + blob.sha;
        const std::string output_path = (root / "subject.txt").string();
        if (!run(revision_command + " > \"" + output_path + "\"")) {
            std::cerr << "Could not resolve commit metadata for " << blob.filepath << '\n';
            fs::remove_all(root, ec);
            return 1;
        }
        std::ifstream subject_file(output_path);
        std::string subject;
        std::getline(subject_file, subject);
        if (subject != found->second || blob.author_name != "GitTrace Test" ||
            blob.author_email != "gittrace-test@example.invalid" || blob.time_mod <= 0) {
            std::cerr << "Incorrect commit metadata for " << blob.filepath << '\n';
            fs::remove_all(root, ec);
            return 1;
        }
        std::cout << "Retrieved " << blob.filepath << " from '" << subject
                  << "' by " << blob.author_name << ": "
                  << std::string(blob.raw_data.begin(), blob.raw_data.end());
        expected.erase(found);
    }

    fs::remove_all(root, ec);
    if (!expected.empty()) {
        std::cerr << "Some expected historical blobs were not retrieved\n";
        return 1;
    }
    std::cout << "GitTrace retrieval test passed (" << trace.blobs.size()
              << " unique historical blobs).\n";
    return 0;
}
