// fetcher.h — libcurl-based HTTP/HTTPS fetcher.
// Each Fetcher instance maintains its own CURL handle; not thread-safe
// across instances, but safe to create one per thread.
#pragma once
#include <string>
#include <curl/curl.h>

struct FetchResult {
    bool        success   = false;
    std::string url;          // final URL after redirects
    std::string content;      // response body
    long        http_code = 0;
    std::string error;
};

class Fetcher {
public:
    static constexpr long        TIMEOUT_SECS  = 15;
    static constexpr long        MAX_REDIRECTS = 5;
    static constexpr long        MAX_BYTES     = 5L * 1024 * 1024;  // 5 MB
    static constexpr const char* USER_AGENT    =
        "Mozilla/5.0 (compatible; CrawlerBot/1.0; +http://example.com/bot)";

    FetchResult fetch(const std::string& url) {
        FetchResult result;
        result.url = url;

        CURL* curl = curl_easy_init();
        if (!curl) {
            result.error = "curl_easy_init failed";
            return result;
        }

        std::string body;
        char err_buf[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        TIMEOUT_SECS);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      MAX_REDIRECTS);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,      USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    err_buf);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE,    MAX_BYTES);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,"");         // auto decompress
        // Accept only HTML
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode rc = curl_easy_perform(curl);

        if (rc == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
            if (result.http_code >= 200 && result.http_code < 300) {
                char* final_url = nullptr;
                curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
                if (final_url) result.url = final_url;
                result.content = std::move(body);
                result.success = true;
            } else {
                result.error = "HTTP " + std::to_string(result.http_code);
            }
        } else {
            result.error = err_buf[0] ? err_buf : curl_easy_strerror(rc);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return result;
    }

private:
    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* data) {
        auto* buf = static_cast<std::string*>(data);
        size_t incoming = size * nmemb;
        if (buf->size() + incoming > static_cast<size_t>(MAX_BYTES))
            return 0;  // returning 0 causes curl to abort with CURLE_WRITE_ERROR
        buf->append(ptr, incoming);
        return incoming;
    }
};
