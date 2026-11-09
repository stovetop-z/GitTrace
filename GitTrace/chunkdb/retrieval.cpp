#include "retrieval.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
bool endsWith(const std::string& value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string normalizeSearchTerm(std::string term)
{
    if (term.size() > 5 && endsWith(term, "ies")) {
        term.resize(term.size() - 3);
        term.push_back('y');
    } else if (term.size() > 5 && endsWith(term, "ing")) {
        term.resize(term.size() - 3);
        if (term.size() > 2 && term.back() == term[term.size() - 2]) term.pop_back();
    } else if (term.size() > 5 && endsWith(term, "ied")) {
        term.resize(term.size() - 3);
        term.push_back('y');
    } else if (term.size() > 4 && endsWith(term, "ed")) {
        term.resize(term.size() - 2);
        if (term == "ignor" || term == "retriev" || term == "us" || term == "creat" ||
            term == "remov" || term == "provid") term.push_back('e');
    } else if (term.size() > 5 && (endsWith(term, "sses") || endsWith(term, "shes") ||
                                    endsWith(term, "ches") || endsWith(term, "xes") ||
                                    endsWith(term, "zes"))) {
        term.resize(term.size() - 2);
    } else if (term.size() > 3 && term.back() == 's') {
        term.pop_back();
    }
    return term;
}

std::unordered_map<std::string, double> weightedQueryTerms(
    std::string_view query, bool expand_query_terms)
{
    std::unordered_map<std::string, double> weighted;
    const std::vector<std::string> terms = gittrace::retrieval::tokenize(query);
    for (const std::string& term : terms) weighted[term] = 1.0;
    if (!expand_query_terms) return weighted;

    static const std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> expansions = {
        {"calculation", {{"formula", 0.45}, {"score", 0.35}}},
        {"downweight", {{"decay", 0.65}, {"penalty", 0.4}}},
        {"older", {{"historical", 0.5}, {"previous", 0.45}}},
        {"revision", {{"commit", 0.45}, {"history", 0.4}}},
        {"ignore", {{"exclude", 0.45}, {"skip", 0.45}}},
        {"rule", {{"pattern", 0.35}, {"filter", 0.35}}},
        {"apply", {{"check", 0.3}}},
        {"throughout", {{"across", 0.35}, {"recursive", 0.35}}},
        {"walk", {{"tree", 0.4}, {"traversal", 0.45}, {"recursive", 0.35}}},
        {"override", {{"injection", 0.5}, {"untrusted", 0.4}}},
        {"instruction", {{"prompt", 0.6}, {"policy", 0.35}}},
        {"snippet", {{"chunk", 0.55}, {"candidate", 0.3}}},
        {"rerank", {{"reranker", 0.55}, {"relevance", 0.4}}},
        {"reranker", {{"relevance", 0.5}}},
        {"cannot", {{"unavailable", 0.35}, {"timeout", 0.35}}},
        {"reach", {{"available", 0.35}, {"timeout", 0.35}}},
        {"display", {{"show", 0.4}, {"print", 0.35}, {"output", 0.35}}}
    };
    for (const std::string& term : terms) {
        const auto found = expansions.find(term);
        if (found == expansions.end()) continue;
        for (const auto& [expanded, weight] : found->second) {
            auto& current_weight = weighted[expanded];
            current_weight = std::max(current_weight, weight);
        }
    }
    return weighted;
}
}

namespace gittrace::retrieval
{
std::vector<std::string> tokenize(std::string_view text)
{
    static const std::unordered_set<std::string> stop_words = {
        "a", "an", "and", "are", "as", "at", "be", "by", "can", "do", "does",
        "for", "from", "how", "if", "in", "into", "is", "it", "of", "on", "or",
        "the", "this", "that", "to", "was", "what", "when", "where", "which", "with"
    };

    std::vector<std::string> terms;
    std::string term;
    auto finish_term = [&]() {
        if (term.size() > 1 && !stop_words.count(term)) {
            const std::string normalized = normalizeSearchTerm(term);
            if (!normalized.empty() && !stop_words.count(normalized)) terms.push_back(normalized);
        }
        term.clear();
    };
    unsigned char previous = 0;
    for (unsigned char ch : text) {
        const bool camel_boundary = std::isupper(ch) &&
            (std::islower(previous) || std::isdigit(previous));
        if (camel_boundary) finish_term();
        if (std::isalnum(ch)) term.push_back(static_cast<char>(std::tolower(ch)));
        else finish_term();
        previous = ch;
    }
    finish_term();
    return terms;
}

std::vector<double> bm25Scores(std::string_view query,
                               const std::vector<std::string>& documents,
                               bool expand_query_terms)
{
    const std::size_t count = documents.size();
    std::vector<double> scores(count, 0.0);
    if (count == 0) return scores;

    const auto query_terms = weightedQueryTerms(query, expand_query_terms);
    std::vector<std::unordered_map<std::string, std::size_t>> frequencies(count);
    std::vector<std::size_t> document_lengths(count, 0);
    std::unordered_map<std::string, std::size_t> document_frequency;
    std::size_t total_length = 0;
    for (std::size_t i = 0; i < count; ++i) {
        for (const std::string& term : tokenize(documents[i])) ++frequencies[i][term];
        document_lengths[i] = std::accumulate(
            frequencies[i].begin(), frequencies[i].end(), std::size_t{0},
            [](std::size_t length, const auto& item) { return length + item.second; });
        total_length += document_lengths[i];
        for (const auto& [term, frequency] : frequencies[i]) {
            (void) frequency;
            ++document_frequency[term];
        }
    }

    const double average_length = static_cast<double>(total_length) / count;
    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    for (const auto& [term, query_weight] : query_terms) {
        const auto df_it = document_frequency.find(term);
        if (df_it == document_frequency.end()) continue;
        const double df = static_cast<double>(df_it->second);
        const double idf = std::log(1.0 + (static_cast<double>(count) - df + 0.5) / (df + 0.5));
        for (std::size_t i = 0; i < count; ++i) {
            const auto tf_it = frequencies[i].find(term);
            if (tf_it == frequencies[i].end()) continue;
            const double tf = static_cast<double>(tf_it->second);
            const double length_ratio = average_length == 0.0
                ? 0.0 : static_cast<double>(document_lengths[i]) / average_length;
            scores[i] += query_weight * idf * (tf * (k1 + 1.0)) /
                (tf + k1 * (1.0 - b + b * length_ratio));
        }
    }
    return scores;
}
}
