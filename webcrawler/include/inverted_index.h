// inverted_index.h — Thread-safe in-memory inverted index with disk persistence.
//
// On-disk layout (inside <out_dir>/):
//   docs.tsv      — docid \t url \t filepath \t depth
//   postings.tsv  — term  \t docid,freq \t docid,freq \t ...
//   dict.tsv      — term  \t df   (document frequency, one line per term)
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

struct DocMetadata {
    int         docid    = -1;
    std::string url;
    std::string filepath;
    int         depth    = 0;
};

class InvertedIndex {
public:
    // Add a document and its token list (thread-safe).
    void add_document(int docid,
                      const std::string& url,
                      const std::string& filepath,
                      int depth,
                      const std::vector<std::string>& tokens) {
        std::unordered_map<std::string, int> freq;
        freq.reserve(tokens.size());
        for (const auto& t : tokens) ++freq[t];

        std::lock_guard<std::mutex> lk(mutex_);
        docs_[docid] = {docid, url, filepath, depth};
        for (const auto& [term, tf] : freq)
            index_[term].emplace_back(docid, tf);
    }

    // Save all index data to <out_dir>/docs.tsv, postings.tsv, dict.tsv.
    void save(const std::string& out_dir) const {
        std::lock_guard<std::mutex> lk(mutex_);

        // docs.tsv
        {
            std::ofstream f(out_dir + "/docs.tsv");
            if (!f) { std::cerr << "[index] Cannot write docs.tsv\n"; return; }
            for (const auto& [id, m] : docs_)
                f << m.docid << '\t' << m.url << '\t'
                  << m.filepath << '\t' << m.depth << '\n';
        }

        // postings.tsv  (term \t docid,freq \t ...)
        {
            std::ofstream f(out_dir + "/postings.tsv");
            if (!f) { std::cerr << "[index] Cannot write postings.tsv\n"; return; }
            for (const auto& [term, postings] : index_) {
                f << term;
                for (const auto& [docid, tf] : postings)
                    f << '\t' << docid << ',' << tf;
                f << '\n';
            }
        }

        // dict.tsv  (term \t df)
        {
            std::ofstream f(out_dir + "/dict.tsv");
            if (!f) { std::cerr << "[index] Cannot write dict.tsv\n"; return; }
            for (const auto& [term, postings] : index_)
                f << term << '\t' << postings.size() << '\n';
        }

        std::cout << "[index] Saved " << docs_.size()  << " docs, "
                  << index_.size() << " unique terms.\n";
    }

    // Load index from <index_dir>/docs.tsv and postings.tsv.
    void load(const std::string& index_dir) {
        load_docs(index_dir + "/docs.tsv");
        load_postings(index_dir + "/postings.tsv");
    }

    // AND intersection query: returns sorted docids matching ALL terms.
    std::vector<int> query_and(const std::vector<std::string>& terms) const {
        if (terms.empty()) return {};

        std::vector<int> result;
        bool first = true;

        for (const auto& term : terms) {
            auto it = index_.find(term);
            if (it == index_.end()) return {};  // term absent → AND is empty

            std::vector<int> ids;
            ids.reserve(it->second.size());
            for (const auto& [docid, _] : it->second) ids.push_back(docid);
            std::sort(ids.begin(), ids.end());

            if (first) {
                result = std::move(ids);
                first  = false;
            } else {
                std::vector<int> inter;
                std::set_intersection(result.begin(), result.end(),
                                      ids.begin(), ids.end(),
                                      std::back_inserter(inter));
                result = std::move(inter);
            }
            if (result.empty()) return {};
        }
        return result;
    }

    const DocMetadata* get_doc(int docid) const {
        auto it = docs_.find(docid);
        return (it != docs_.end()) ? &it->second : nullptr;
    }

    size_t num_docs()  const { std::lock_guard<std::mutex> lk(mutex_); return docs_.size();  }
    size_t num_terms() const { std::lock_guard<std::mutex> lk(mutex_); return index_.size(); }

private:
    void load_docs(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "[index] Cannot open " << path << '\n'; return; }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string id_s, depth_s;
            DocMetadata m;
            std::getline(ss, id_s,       '\t');
            std::getline(ss, m.url,      '\t');
            std::getline(ss, m.filepath, '\t');
            std::getline(ss, depth_s,    '\t');
            try {
                m.docid = std::stoi(id_s);
                m.depth = std::stoi(depth_s);
                docs_[m.docid] = m;
            } catch (...) {}
        }
    }

    void load_postings(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "[index] Cannot open " << path << '\n'; return; }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string term, posting;
            std::getline(ss, term, '\t');
            while (std::getline(ss, posting, '\t')) {
                if (posting.empty()) continue;
                size_t comma = posting.find(',');
                if (comma == std::string::npos) continue;
                try {
                    int docid = std::stoi(posting.substr(0, comma));
                    int tf    = std::stoi(posting.substr(comma + 1));
                    index_[term].emplace_back(docid, tf);
                } catch (...) {}
            }
        }
    }

    mutable std::mutex mutex_;
    // term → [(docid, tf)]
    std::unordered_map<std::string, std::vector<std::pair<int,int>>> index_;
    std::unordered_map<int, DocMetadata> docs_;
};
