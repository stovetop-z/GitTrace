#include "laya_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

using nlohmann::json;

namespace
{
std::size_t append_response(char* data, std::size_t size, std::size_t count, void* user_data)
{
    const std::size_t bytes = size * count;
    static_cast<std::string*>(user_data)->append(data, bytes);
    return bytes;
}

bool initialize_curl(std::string& error)
{
    static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }
    return true;
}
}

LayaClient::LayaClient(std::string endpoint) : endpoint(std::move(endpoint))
{
}

bool LayaClient::rerank(const std::string& query,
                        const std::vector<ChunkMatch>& candidates,
                        std::vector<LayaMatch>& results,
                        std::string& error) const
{
    results.clear();
    if (candidates.empty()) return true;
    if (!initialize_curl(error)) return false;

    json request;
    request["query"] = query;
    request["min_relevance"] = "related";
    request["candidates"] = json::array();

    std::unordered_map<std::string, ChunkMatch> by_id;
    by_id.reserve(candidates.size());
    for (const ChunkMatch& candidate : candidates) {
        if (!candidate.chunk) continue;
        const Chunk& chunk = *candidate.chunk;
        const std::string id = std::to_string(chunk.id);
        by_id.emplace(id, candidate);

        json item = {
            {"id", id},
            {"path", chunk.filepath},
            {"text", chunk.input.substr(0, 12000)},
            {"similarity", candidate.similarity},
            {"commit_sha", chunk.commit_sha},
            {"start_line", chunk.start_line},
            {"end_line", chunk.end_line}
        };
        request["candidates"].push_back(std::move(item));
    }
    if (request["candidates"].empty()) return true;

    const std::string payload = request.dump(-1, ' ', false, json::error_handler_t::replace);
    std::string response;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
        error = "could not create HTTP client";
        return false;
    }

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);

    curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        error = "Reranker service returned HTTP " + std::to_string(status);
        if (!response.empty()) error += ": " + response;
        return false;
    }

    try {
        const json document = json::parse(response);
        if (!document.contains("results") || !document["results"].is_array()) {
            error = "Laya response has no results array";
            return false;
        }

        results.reserve(document["results"].size());
        for (const json& item : document["results"]) {
            const std::string id = item.at("id").get<std::string>();
            const auto candidate = by_id.find(id);
            if (candidate == by_id.end()) continue;

            LayaMatch match;
            match.match = candidate->second;
            match.relevance = item.value("relevance", std::string("irrelevant"));
            match.confidence = item.value("confidence", 0.0f);
            match.accepted = item.value("accepted", false);
            results.push_back(std::move(match));
        }
    } catch (const std::exception& exception) {
        error = std::string("could not parse Laya response: ") + exception.what();
        results.clear();
        return false;
    }

    if (results.size() != by_id.size()) {
        error = "Laya response did not include every submitted candidate";
        results.clear();
        return false;
    }
    return true;
}
