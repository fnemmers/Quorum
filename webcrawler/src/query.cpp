// query.cpp — Inverted-index query tool.
//
// Loads the on-disk index produced by the indexer, then performs an
// AND-intersection search for the supplied terms and prints the matching
// documents (docid, URL, local filepath).
//
// Usage:
//   ./query --index <dir> <term1> [term2 ...]
//
// Example:
//   ./query --index data/index  web crawler
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>

#include "inverted_index.h"  // InvertedIndex: load(), query_and(), get_doc()
#include "tokenizer.h"       // tokenizer::tokenize — normalises query terms to match index form

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --index <dir> <term1> [term2 ...]\n"
              << "  Performs AND search across the persistent inverted index.\n";
}

int main(int argc, char* argv[]) {
    // Need at least: ./query --index <dir> <term>  → 4 args minimum
    if (argc < 4) { print_usage(argv[0]); return 1; }

    std::string index_dir;
    std::vector<std::string> raw_terms;  // query words as the user typed them

    // Walk argv: consume --index <dir>, treat everything else as a query term
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--index" && i + 1 < argc)
            index_dir = argv[++i];  // advance i to skip the value on the next iteration
        else
            raw_terms.emplace_back(argv[i]);
    }

    if (index_dir.empty() || raw_terms.empty()) {
        print_usage(argv[0]); return 1;
    }

    // Normalise query terms through the same tokeniser used at index time so
    // that e.g. "Web" and "WEB" both match "web" in the index.
    // filter_stops=false so the user can explicitly search for stopwords.
    std::vector<std::string> terms;
    for (const auto& raw : raw_terms) {
        auto toks = tokenizer::tokenize(raw, /*filter_stops=*/false);
        for (auto& t : toks) terms.push_back(std::move(t));
    }
    // Remove duplicate terms so we don't AND a term against itself
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

    if (terms.empty()) {
        std::cerr << "No valid query terms after normalisation.\n"; return 1;
    }

    // Load the persistent index written by the indexer into memory
    InvertedIndex index;
    std::cout << "Loading index from " << index_dir << " ...\n";
    index.load(index_dir);
    std::cout << "Index stats: " << index.num_docs()  << " documents, "
              << index.num_terms() << " unique terms\n\n";

    // Print the normalised query so the user can see what was actually searched
    std::cout << "Query (AND): ";
    for (const auto& t : terms) std::cout << '"' << t << "\" ";
    std::cout << "\n" << std::string(70, '-') << "\n";

    // AND query: returns only docids whose postings lists contain *all* terms.
    // The intersection is computed inside InvertedIndex::query_and.
    auto results = index.query_and(terms);

    if (results.empty()) {
        std::cout << "No documents matched.\n";
        return 0;
    }

    std::cout << results.size() << " result(s):\n\n";
    int rank = 1;
    for (int docid : results) {
        // get_doc looks up the document metadata (url, filepath, depth) by docid
        const auto* m = index.get_doc(docid);
        if (!m) continue;  // shouldn't happen, but guard against a corrupt index
        std::cout << std::setw(4) << rank++ << ".  "
                  << "[doc " << std::setw(4) << m->docid << "]  "
                  << m->url << "\n"
                  << "          file:  " << m->filepath
                  << "  depth: " << m->depth << "\n\n";
    }
    return 0;
}
