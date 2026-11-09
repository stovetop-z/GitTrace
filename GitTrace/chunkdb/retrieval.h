#ifndef GITTRACE_RETRIEVAL_H
#define GITTRACE_RETRIEVAL_H

#include <string>
#include <string_view>
#include <vector>

namespace gittrace::retrieval
{
std::vector<std::string> tokenize(std::string_view text);

std::vector<double> bm25Scores(std::string_view query,
                               const std::vector<std::string>& documents,
                               bool expand_query_terms = false);
}

#endif
