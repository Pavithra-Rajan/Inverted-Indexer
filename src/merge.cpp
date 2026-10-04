#include "varbyte.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Returns the number of runs from the stats.txt file in the given directory.
static size_t read_num_runs(const std::string &runs_dir) {
    std::ifstream stats(runs_dir + "/stats.txt");
    std::string key;
    uint64_t value;
    while (stats >> key >> value) {
        if (key == "num_runs") return (size_t)value;
    }
    return 0;
}

// Class containing state for one run file. 
// It is mainly used for grouping together all postings for the same term.
struct RunReader {
    std::ifstream in;
    bool has_pending = false;
    std::string pending_term;
    uint32_t pending_docid = 0, pending_freq = 0;

    std::string cur_term;
    std::vector<std::pair<uint32_t, uint32_t>> group;

    explicit RunReader(const std::string &path) : in(path) { advance(); }

    void advance() {
        has_pending = (bool)(in >> pending_term >> pending_docid >> pending_freq);
    }

    bool next_group() {
        if (!has_pending) return false;
        cur_term = pending_term;
        group.clear();
        while (has_pending && pending_term == cur_term) {
            group.emplace_back(pending_docid, pending_freq);
            advance();
        }
        return true;
    }
};

// One heap item stores current term along with the run it came from.
struct HeapItem {
    std::string term;
    size_t run_idx;
};

// Comparator to be used for the min-heap (priority queue) to merge the runs.
// Since by default std::priority_queue is a max-heap, we need to reverse the comparison to get a min-heap.
// First elements are compared lexicographically, and run indexes are compared numerically to break ties.
struct HeapCmp {
    bool operator()(const HeapItem &a, const HeapItem &b) const {
        if (a.term != b.term) return a.term > b.term;
        return a.run_idx > b.run_idx;
    }
};

// Writes a term's postings in a compressed(varbyte) format to the index file and its metadata to the lexicon file.
static void write_term(std::ofstream &index_out, std::ofstream &lexicon_out,
                        const std::string &term,
                        const std::vector<std::pair<uint32_t, uint32_t>> &postings,
                        size_t block_size) {

    // Tells us where we are in the index_out file
    uint64_t offset = (uint64_t)index_out.tellp();

    // The document frequency is the number of postings for this term.
    uint32_t df = (uint32_t)postings.size();

    // To determine the ceiling of the number of blocks we need to create.
    uint32_t num_blocks = (uint32_t)((postings.size() + block_size - 1) / block_size);

    // Vector containing the compressed docid for each block and another vector containing the compressed frequency.
    std::vector<std::vector<uint8_t>> docid_blocks(num_blocks), freq_blocks(num_blocks);

    // Vector containg the last docid of each block.
    std::vector<uint32_t> block_last_docid(num_blocks);

    for (uint32_t b = 0; b < num_blocks; b++) {
        size_t start = (size_t)b * block_size;

        //to account for a smaller last block if postings.size() is not a multiple of block_size
        size_t end = std::min(start + block_size, postings.size()); 

        uint32_t prev_docid = 0;
        for (size_t i = start; i < end; i++) {
            uint32_t docid = postings[i].first;
            // Since docids are sorted, we store only the gap between the current docid and the previous one.
            varbyte_encode(docid - prev_docid, docid_blocks[b]);
            // storing frequency
            varbyte_encode(postings[i].second, freq_blocks[b]);
            prev_docid = docid;
        }
        //store the greatest docid in the block to be used for skipping during query processing
        block_last_docid[b] = postings[end - 1].first;
    }

    // Header for the term in the index file: df, num_blocks
    index_out.write((char *)&df, sizeof(df));
    index_out.write((char *)&num_blocks, sizeof(num_blocks));

     // For each block: last_docid, docid_bytes, freq_bytes
    for (uint32_t b = 0; b < num_blocks; b++) {
        uint32_t docid_bytes = (uint32_t)docid_blocks[b].size();
        uint32_t freq_bytes = (uint32_t)freq_blocks[b].size();
        index_out.write((char *)&block_last_docid[b], sizeof(uint32_t));
        index_out.write((char *)&docid_bytes, sizeof(uint32_t));
        index_out.write((char *)&freq_bytes, sizeof(uint32_t));
    }

    // Write the actual docid and frequency blocks to the index file.
    for (uint32_t b = 0; b < num_blocks; b++)
        index_out.write((char *)docid_blocks[b].data(), docid_blocks[b].size());
    for (uint32_t b = 0; b < num_blocks; b++)
        index_out.write((char *)freq_blocks[b].data(), freq_blocks[b].size());

    // Write the term, offset, and df to the lexicon file.
    lexicon_out << term << '\t' << offset << '\t' << df << '\n';
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <runs_dir> <out_dir> [block_size]\n";
        return 1;
    }
    std::string runs_dir = argv[1];
    std::string out_dir = argv[2];
    size_t block_size = argc > 3 ? std::stoul(argv[3]) : 128;

    // Find number of runs from stats.txt
    size_t num_runs = read_num_runs(runs_dir);
    if (num_runs == 0) {
        std::cerr << "no runs found (check " << runs_dir << "/stats.txt)\n";
        return 1;
    }

    std::ios::sync_with_stdio(false);
    auto start = Clock::now();

    // Create a list of RunReader objects, one for each run file and reserve space for them in the vector.
    std::vector<RunReader> readers;
    readers.reserve(num_runs);

    // Open each run file and create a RunReader for it, adding it to the readers vector.
    for (size_t i = 0; i < num_runs; i++)
        readers.emplace_back(runs_dir + "/run_" + std::to_string(i) + ".txt");

    // Create a min-heap (priority queue) to keep track of the current head group of each run.
    // A single heap item will contain the {current term, run index} of the head group. 
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCmp> heap;
    for (size_t i = 0; i < num_runs; i++)
        if (readers[i].next_group()) heap.push({readers[i].cur_term, i});

    //opening the output files for the merged index and lexicon
    std::ofstream index_out(out_dir + "/index.bin", std::ios::binary);
    std::ofstream lexicon_out(out_dir + "/lexicon.txt");

    std::string current_term;
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    uint64_t num_terms = 0;

    while (!heap.empty()) {
        // Get the smallest term from the heap
        HeapItem top = heap.top();
        heap.pop();

        // If there is term change, write the previous term's postings to the index and lexicon files.
        if (!current_term.empty() && top.term != current_term) {
            write_term(index_out, lexicon_out, current_term, merged, block_size);
            merged.clear();
            num_terms++;
        }

        // set current_term to the term of the top heap item
        current_term = top.term;

        // Append the postings of the current term from the run to the merged list
        auto &g = readers[top.run_idx].group;
        merged.insert(merged.end(), g.begin(), g.end());

        // Push the next group from the same run into the heap if it exists
        if (readers[top.run_idx].next_group())
            heap.push({readers[top.run_idx].cur_term, top.run_idx});
    }

    // Write any pending term's postings to the index and lexicon files after the heap is empty.
    if (!merged.empty()) {
        write_term(index_out, lexicon_out, current_term, merged, block_size);
        num_terms++;
    }

    std::cerr << std::fixed << std::setprecision(2)
               << "merged " << num_runs << " runs into " << num_terms
               << " terms, total time " << seconds_since(start) << "s\n";
    return 0;
}
