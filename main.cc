#include "GitTrace/gittrace.h"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc > 2)
    {
        std::cerr << "Usage: " << argv[0] << " [repository-path]\n";
        return 2;
    }

    const char* repositoryPath = argc == 2 ? argv[1] : ".";

    GitTrace trace;
    std::cout << "Opening repository: " << repositoryPath << '\n';
    if (!trace.init(repositoryPath, true))
    {
        std::cerr << "Failed to initialize GitTrace. Check the repository path "
                     "and embedding model configuration.\n";
        return 1;
    }

    std::cout << "Reading Git history and parsing supported files...\n";
    trace.beginGitTrace();

    std::cout << "GitTrace finished. Collected " << trace.blobs.size()
              << " unique text blob version(s).\n";
    return 0;
}
