#include "tokenizer.hpp"
#include "varbyte.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

static const uint32_t MAX_DOCID = UINT32_MAX;
static const double K1 = 1.2, B = 0.75;
static const size_t TOP_K = 10;

struct LexEntry {
    uint64_t offset;
    uint32_t df;
};

struct BlockMeta {
    uint32_t last_docid, docid_bytes, freq_bytes;
};

struct ListPointer {
    uint32_t df = 0;
    double idf = 0;
    std::vector<BlockMeta> blocks;
    std::vector<uint8_t> docid_data, freq_data;  // whole list, still compressed
    std::vector<uint32_t> docids, freqs;         // current block only, decompressed
    size_t block = 0, pos = 0;
    size_t docid_off = 0, freq_off = 0;
    bool docids_loaded = false, freqs_loaded = false;
};

struct Index {
    std::ifstream index_in;
    std::unordered_map<std::string, LexEntry> lexicon;
    std::vector<uint32_t> page_table, doc_lengths;
    uint32_t num_docs = 0;
    double avg_doc_len = 0;
};

using Result = std::pair<double, uint32_t>;
using TopK = std::priority_queue<Result, std::vector<Result>, std::greater<Result>>;

static double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

template <typename T>
static bool read_binary_file(const std::string &path, std::vector<T> &out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    out.resize((size_t)in.tellg() / sizeof(T));
    in.seekg(0);
    in.read((char *)out.data(), (std::streamsize)(out.size() * sizeof(T)));
    return true;
}

static bool load_index(Index &idx, const std::string &runs_dir, const std::string &index_dir) {
    std::ifstream stats(runs_dir + "/stats.txt");
    std::string key;
    uint64_t value, total_tokens = 0;
    while (stats >> key >> value) {
        if (key == "num_docs") idx.num_docs = (uint32_t)value;
        if (key == "total_tokens") total_tokens = value;
    }
    if (idx.num_docs == 0) return false;
    idx.avg_doc_len = (double)total_tokens / idx.num_docs;

    if (!read_binary_file(runs_dir + "/page_table.bin", idx.page_table)) return false;
    if (!read_binary_file(runs_dir + "/doc_lengths.bin", idx.doc_lengths)) return false;

    std::ifstream lexicon_in(index_dir + "/lexicon.txt");
    if (!lexicon_in) return false;
    std::string term;
    LexEntry entry;
    while (lexicon_in >> term >> entry.offset >> entry.df) idx.lexicon.emplace(term, entry);

    idx.index_in.open(index_dir + "/index.bin", std::ios::binary);
    return (bool)idx.index_in;
}

// Reads the compressed list into memory; nothing is decompressed until nextGEQ needs a block.
static ListPointer openList(Index &idx, const LexEntry &entry) {
    ListPointer lp;
    uint32_t num_blocks;
    idx.index_in.seekg((std::streamoff)entry.offset);
    idx.index_in.read((char *)&lp.df, sizeof(lp.df));
    idx.index_in.read((char *)&num_blocks, sizeof(num_blocks));
    lp.blocks.resize(num_blocks);
    idx.index_in.read((char *)lp.blocks.data(), (std::streamsize)(num_blocks * sizeof(BlockMeta)));

    size_t docid_bytes = 0, freq_bytes = 0;
    for (auto &b : lp.blocks) {
        docid_bytes += b.docid_bytes;
        freq_bytes += b.freq_bytes;
    }
    lp.docid_data.resize(docid_bytes);
    lp.freq_data.resize(freq_bytes);
    idx.index_in.read((char *)lp.docid_data.data(), (std::streamsize)docid_bytes);
    idx.index_in.read((char *)lp.freq_data.data(), (std::streamsize)freq_bytes);

    // Lucene-style idf (+1 inside the log) so terms in more than half the docs don't score negative.
    lp.idf = std::log(1.0 + (idx.num_docs - lp.df + 0.5) / (lp.df + 0.5));
    return lp;
}

static void closeList(ListPointer &lp) {
    lp = ListPointer();
}

// Returns the smallest docID >= k in the list, or MAX_DOCID if there is none.
static uint32_t nextGEQ(ListPointer &lp, uint32_t k) {
    size_t b = lp.block;
    while (b < lp.blocks.size() && lp.blocks[b].last_docid < k) {
        lp.docid_off += lp.blocks[b].docid_bytes;
        lp.freq_off += lp.blocks[b].freq_bytes;
        b++;
    }
    if (b != lp.block) {
        lp.block = b;
        lp.docids_loaded = false;
    }
    if (b == lp.blocks.size()) return MAX_DOCID;

    if (!lp.docids_loaded) {
        const uint8_t *p = lp.docid_data.data() + lp.docid_off;
        const uint8_t *end = p + lp.blocks[b].docid_bytes;
        lp.docids.clear();
        // Gaps restart from 0 in every block.
        uint32_t docid = 0;
        while (p < end) {
            docid += varbyte_decode(p);
            lp.docids.push_back(docid);
        }
        lp.pos = 0;
        lp.docids_loaded = true;
        lp.freqs_loaded = false;
    }
    while (lp.docids[lp.pos] < k) lp.pos++;
    return lp.docids[lp.pos];
}

// Frequency of the posting nextGEQ last returned and the freq block is decompressed only on first use.
static uint32_t getFreq(ListPointer &lp) {
    if (!lp.freqs_loaded) {
        const uint8_t *p = lp.freq_data.data() + lp.freq_off;
        lp.freqs.resize(lp.docids.size());
        for (auto &f : lp.freqs) f = varbyte_decode(p);
        lp.freqs_loaded = true;
    }
    return lp.freqs[lp.pos];
}

static double bm25(const Index &idx, ListPointer &lp, uint32_t docid) {
    double freq = getFreq(lp);
    double norm = K1 * ((1 - B) + B * idx.doc_lengths[docid] / idx.avg_doc_len);
    return lp.idf * freq * (K1 + 1) / (freq + norm);
}

static void offer(TopK &top, double score, uint32_t docid) {
    if (top.size() < TOP_K) {
        top.emplace(score, docid);
    } else if (score > top.top().first) {
        top.pop();
        top.emplace(score, docid);
    }
}

static void conjunctive(const Index &idx, std::vector<ListPointer> &lists, TopK &top) {
    // Shortest list drives, so the longer lists are only probed with nextGEQ.
    std::sort(lists.begin(), lists.end(),
              [](const ListPointer &a, const ListPointer &b) { return a.df < b.df; });

    uint32_t did = 0;
    while (true) {
        did = nextGEQ(lists[0], did);
        if (did == MAX_DOCID) break;

        uint32_t d = did;
        for (size_t i = 1; i < lists.size() && d == did; i++) d = nextGEQ(lists[i], did);
        if (d == MAX_DOCID) break;
        if (d > did) {
            did = d;
            continue;
        }

        double score = 0;
        for (auto &lp : lists) score += bm25(idx, lp, did);
        offer(top, score, did);
        did++;
    }
}

static void disjunctive(const Index &idx, std::vector<ListPointer> &lists, TopK &top) {
    std::vector<uint32_t> cur(lists.size());
    for (size_t i = 0; i < lists.size(); i++) cur[i] = nextGEQ(lists[i], 0);

    while (true) {
        uint32_t did = *std::min_element(cur.begin(), cur.end());
        if (did == MAX_DOCID) break;

        double score = 0;
        for (size_t i = 0; i < lists.size(); i++) {
            if (cur[i] != did) continue;
            score += bm25(idx, lists[i], did);
            cur[i] = nextGEQ(lists[i], did + 1);
        }
        offer(top, score, did);
    }
}

// Same tokenizer as the parser; repeated query terms count once.
static std::vector<std::string> query_terms(const std::string &query) {
    std::vector<std::string> terms;
    tokenize(query, terms);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

// Returns the top results, best first.
static std::vector<Result> run_query(Index &idx, const std::vector<std::string> &terms, bool conj) {
    std::vector<ListPointer> lists;
    bool missing = false;
    for (auto &term : terms) {
        auto it = idx.lexicon.find(term);
        if (it == idx.lexicon.end()) {
            missing = true;
            continue;
        }
        lists.push_back(openList(idx, it->second));
    }

    // A term absent from the lexicon empties an AND query; an OR query just ignores it.
    TopK top;
    if (!lists.empty() && !(conj && missing)) {
        if (conj) conjunctive(idx, lists, top);
        else disjunctive(idx, lists, top);
    }
    for (auto &lp : lists) closeList(lp);

    std::vector<Result> results;
    for (; !top.empty(); top.pop()) results.push_back(top.top());
    std::reverse(results.begin(), results.end());
    return results;
}

static void command_line(Index &idx) {
    std::string line, mode;
    while (true) {
        std::cout << "\nquery> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::vector<std::string> terms = query_terms(line);
        if (terms.empty()) continue;

        // Anything other than "c" means disjunctive.
        std::cout << "mode (c = conjunctive, d = disjunctive)> " << std::flush;
        if (!std::getline(std::cin, mode)) break;
        bool conj = !mode.empty() && (mode[0] == 'c' || mode[0] == 'C');

        auto start = Clock::now();
        std::vector<Result> results = run_query(idx, terms, conj);

        std::cout << std::fixed << std::setprecision(4);
        if (results.empty()) std::cout << "no results\n";
        for (size_t i = 0; i < results.size(); i++) {
            std::cout << std::setw(2) << (i + 1) << ".  score " << results[i].first
                      << "  passage " << idx.page_table[results[i].second] << '\n';
        }
        std::cout << (conj ? "conjunctive" : "disjunctive") << ", "
                  << (seconds_since(start) * 1000) << " ms\n";
    }
    std::cout << '\n';
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <runs_dir> <index_dir>\n";
        return 1;
    }

    Index idx;
    auto load_start = Clock::now();
    if (!load_index(idx, argv[1], argv[2])) {
        std::cerr << "cannot load index from " << argv[1] << " and " << argv[2] << "\n";
        return 1;
    }

    std::cerr << std::fixed << std::setprecision(2)
              << "loaded " << idx.lexicon.size() << " terms, " << idx.num_docs << " docs in "
              << seconds_since(load_start) << "s\n";

    command_line(idx);
    return 0;
}
