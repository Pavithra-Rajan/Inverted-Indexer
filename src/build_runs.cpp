#include "tokenizer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using Postings = std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>;
using Clock = std::chrono::steady_clock;


// Returns the number of seconds since the given start time.
static double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Writes the block to disk and prints its postings/size/time stats.
static void flush_run(Postings &block, size_t block_postings, size_t run_index,
                       const std::string &out_dir, Clock::time_point block_start) {
    std::vector<std::string> terms;
    terms.reserve(block.size());

    // 1. Collect all the terms in the block and sort them
    for (auto &kv : block) terms.push_back(kv.first);
    std::sort(terms.begin(), terms.end());

    // 2. 
    std::ofstream out(out_dir + "/run_" + std::to_string(run_index) + ".txt");
    for (auto &term : terms) {
        for (auto &posting : block[term]) {
            out << term << '\t' << posting.first << '\t' << posting.second << '\n';
        }
    }
    size_t bytes = (size_t)out.tellp();

    std::cerr << std::fixed << std::setprecision(2)
               << "run " << run_index << ": " << block_postings << " postings, "
               << (bytes / 1e6) << " MB, " << seconds_since(block_start) << "s\n";
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <collection.tsv> <out_dir> [buffer_threshold]\n";
        return 1;
    }
    std::string collection_path = argv[1];
    std::string out_dir = argv[2];
    size_t buffer_threshold = argc > 3 ? std::stoul(argv[3]) : 5'000'000;

    std::ios::sync_with_stdio(false);

    std::ifstream in(collection_path);
    if (!in) {
        std::cerr << "cannot open " << collection_path << "\n";
        return 1;
    }
    std::ofstream doc_lengths(out_dir + "/doc_lengths.bin", std::ios::binary);
    std::ofstream page_table(out_dir + "/page_table.bin", std::ios::binary);

    Postings block;
    size_t block_postings = 0;
    size_t run_index = 0;
    uint64_t total_tokens = 0;
    uint32_t doc_id = 0;

    auto program_start = Clock::now();
    auto block_start = program_start;

    std::string line;
    std::vector<std::string> tokens;
    std::unordered_map<std::string, uint32_t> local_freq;

    while (std::getline(in, line)) {

        // 1. Read each line of the collection
        size_t tabIndex = line.find('\t');
        const std::string &text = (tabIndex == std::string::npos) ? line : line.substr(tabIndex + 1);

       // 2. Find passage id or use our sequential doc_id if not present
        uint32_t pid = (tabIndex == std::string::npos) ? doc_id : (uint32_t)std::stoul(line.substr(0, tabIndex));
        
        // 3. Store pid in page table in binary to maintain mapping between doc_id and passage id 
        page_table.write((char *)&pid, sizeof(pid));

        // 4. convert text into list of normalized tokens
        tokenize(text, tokens);

        // 5. Count the frequency of each token in this document
        local_freq.clear();
        for (auto &t : tokens) local_freq[t]++;

        // 6. Write the document length in binary in the doc_lengths file
        uint32_t doc_len = (uint32_t)tokens.size();
        doc_lengths.write((char *)&doc_len, sizeof(doc_len));
        
        total_tokens += doc_len;

        // 7. Add the local frequencies to the block postings list
        // kv.first -> term, kv.second -> frequency of term in this document
        // A block is a map of term -> list of (doc_id, frequency) pairs.
        for (auto &kv : local_freq) {
            block[kv.first].emplace_back(doc_id, kv.second);
        }

        // 8. Update the number of postings in the block
        block_postings += local_freq.size();

        // 9. If the number of postings in the block exceeds the buffer threshold, flush the block to disk
        if (block_postings >= buffer_threshold) {
            flush_run(block, block_postings, run_index++, out_dir, block_start);
            block.clear();
            block_postings = 0;
            block_start = Clock::now();
        }

        doc_id++;
    }
    if (block_postings > 0) {
        flush_run(block, block_postings, run_index++, out_dir, block_start);
    }

    std::ofstream stats(out_dir + "/stats.txt");
    stats << "num_docs\t" << doc_id << '\n';
    stats << "total_tokens\t" << total_tokens << '\n';
    stats << "num_runs\t" << run_index << '\n';

    std::cerr << std::fixed << std::setprecision(2)
               << "parsed " << doc_id << " docs into " << run_index << " runs, total time "
               << seconds_since(program_start) << "s\n";
    return 0;
}
