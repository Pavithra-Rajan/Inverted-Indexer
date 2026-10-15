#include "tokenizer.hpp"
#include "varbyte.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

static const uint32_t MAX_DOCID = UINT32_MAX;
static const double K1 = 1.2, B = 0.75;
static const size_t TOP_K = 10;
static const size_t SNIPPET_WORDS = 25;

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
    size_t term = 0;  // position in the query, since conjunctive reorders the lists
    std::vector<BlockMeta> blocks;
    std::vector<uint8_t> docid_data, freq_data;  // whole list, still compressed
    std::vector<uint32_t> docids, freqs;         // current block only, decompressed
    size_t block = 0, pos = 0;
    size_t docid_off = 0, freq_off = 0;
    bool docids_loaded = false, freqs_loaded = false;
};

struct Index {
    std::ifstream index_in, collection_in;
    std::unordered_map<std::string, LexEntry> lexicon;
    std::vector<uint32_t> page_table, doc_lengths;
    std::vector<uint64_t> doc_offsets;
    uint32_t num_docs = 0;
    double avg_doc_len = 0;
};

// How matched terms are marked in a snippet: ANSI bold in the terminal, and added <b> in the browser.
struct Markup {
    const char *open, *close;
    bool html;
};

struct Result {
    double score;
    uint32_t docid;
    std::vector<double> term_scores;  // one per query term, 0 if the term is not in the doc
};

// Min-heap on (score, docid), so the weakest of the top results sits on top.
struct WorseFirst {
    bool operator()(const Result &a, const Result &b) const {
        return a.score != b.score ? a.score > b.score : a.docid > b.docid;
    }
};
using TopK = std::priority_queue<Result, std::vector<Result>, WorseFirst>;

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
    std::ifstream stats(runs_dir + "/runs/stats.txt");
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

// Passage text is read from the original collection by seeking; it is never held in memory.
static std::string passage_text(Index &idx, uint32_t docid) {
    std::string line;
    idx.collection_in.clear();
    idx.collection_in.seekg((std::streamoff)idx.doc_offsets[docid]);
    std::getline(idx.collection_in, line);
    size_t tab = line.find('\t');
    return tab == std::string::npos ? line : line.substr(tab + 1);
}

static void append_text(std::string &out, const std::string &text, size_t from, size_t len, bool html) {
    if (!html) {
        out.append(text, from, len);
        return;
    }
    for (size_t i = from; i < from + len; i++) {
        switch (text[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += text[i];
        }
    }
}

// Picks the SNIPPET_WORDS-word window with the highest sum of idf over distinct query terms;
// ties go to the window with more query-term occurrences, then to the earliest one.
static std::string snippet(const std::string &text,
                           const std::unordered_map<std::string, double> &term_idf,
                           const Markup &markup, size_t max_words = SNIPPET_WORDS) {
    std::vector<TokenSpan> toks;
    tokenize_spans(text, toks);
    std::string out;
    if (toks.empty()) {
        append_text(out, text, 0, text.size(), markup.html);
        return out;
    }
    size_t n = toks.size(), width = std::min(n, max_words);

    size_t best = 0, best_hits = 0;
    double best_score = -1;
    std::vector<const std::string *> seen;
    for (size_t s = 0; s + width <= n; s++) {
        double score = 0;
        size_t hits = 0;
        seen.clear();
        for (size_t i = s; i < s + width; i++) {
            auto it = term_idf.find(toks[i].term);
            if (it == term_idf.end()) continue;
            hits++;
            if (std::find(seen.begin(), seen.end(), &it->first) == seen.end()) {
                seen.push_back(&it->first);
                score += it->second;
            }
        }
        // Epsilon because the same terms summed in a different order differ in the last bits.
        if (score > best_score + 1e-9 || (score > best_score - 1e-9 && hits > best_hits)) {
            best = s;
            best_score = score;
            best_hits = hits;
        }
    }

    size_t last = best + width;
    size_t p = best == 0 ? 0 : toks[best].start;
    size_t to = last == n ? text.size() : toks[last - 1].end;
    if (best > 0) out += "... ";
    for (size_t i = best; i < last; i++) {
        if (!term_idf.count(toks[i].term)) continue;
        append_text(out, text, p, toks[i].start - p, markup.html);
        out += markup.open;
        append_text(out, text, toks[i].start, toks[i].end - toks[i].start, markup.html);
        out += markup.close;
        p = toks[i].end;
    }
    append_text(out, text, p, to - p, markup.html);
    if (last < n) out += " ...";
    return out;
}

static void offer(TopK &top, double score, uint32_t docid, const std::vector<double> &term_scores) {
    if (top.size() < TOP_K) {
        top.push({score, docid, term_scores});
    } else if (score > top.top().score) {
        top.pop();
        top.push({score, docid, term_scores});
    }
}

static void conjunctive(const Index &idx, std::vector<ListPointer> &lists, size_t num_terms, TopK &top) {
    // Shortest list drives, so the longer lists are only probed with nextGEQ.
    std::sort(lists.begin(), lists.end(),
              [](const ListPointer &a, const ListPointer &b) { return a.df < b.df; });

    std::vector<double> term_scores(num_terms);
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
        for (auto &lp : lists) score += term_scores[lp.term] = bm25(idx, lp, did);
        offer(top, score, did, term_scores);
        did++;
    }
}

static void disjunctive(const Index &idx, std::vector<ListPointer> &lists, size_t num_terms, TopK &top) {
    std::vector<double> term_scores(num_terms);
    std::vector<uint32_t> cur(lists.size());
    for (size_t i = 0; i < lists.size(); i++) cur[i] = nextGEQ(lists[i], 0);

    while (true) {
        uint32_t did = *std::min_element(cur.begin(), cur.end());
        if (did == MAX_DOCID) break;

        double score = 0;
        std::fill(term_scores.begin(), term_scores.end(), 0.0);
        for (size_t i = 0; i < lists.size(); i++) {
            if (cur[i] != did) continue;
            score += term_scores[lists[i].term] = bm25(idx, lists[i], did);
            cur[i] = nextGEQ(lists[i], did + 1);
        }
        offer(top, score, did, term_scores);
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

// Returns the top results, best first, and fills term_idf for the terms found in the lexicon.
static std::vector<Result> run_query(Index &idx, const std::vector<std::string> &terms, bool conj,
                                     std::unordered_map<std::string, double> &term_idf) {
    std::vector<ListPointer> lists;
    bool missing = false;
    for (size_t i = 0; i < terms.size(); i++) {
        auto it = idx.lexicon.find(terms[i]);
        if (it == idx.lexicon.end()) {
            missing = true;
            continue;
        }
        lists.push_back(openList(idx, it->second));
        lists.back().term = i;
        term_idf[terms[i]] = lists.back().idf;
    }

    // A term absent from the lexicon empties an AND query; an OR query just ignores it.
    TopK top;
    if (!lists.empty() && !(conj && missing)) {
        if (conj) conjunctive(idx, lists, terms.size(), top);
        else disjunctive(idx, lists, terms.size(), top);
    }
    for (auto &lp : lists) closeList(lp);

    // Return results from heap in reverse order (greatest -> least scored)
    std::vector<Result> results;
    for (; !top.empty(); top.pop()) results.push_back(top.top());
    std::reverse(results.begin(), results.end());
    return results;
}

static void command_line(Index &idx, bool show_text) {
    // Bold escape codes only when printing to a terminal, to pipe easily to a file.
    bool tty = isatty(STDOUT_FILENO);
    Markup markup = {tty ? "\033[1m" : "", tty ? "\033[0m" : "", false};

    std::string line, mode;
    while (true) {
        // Get query from user 
        std::cout << "\nquery> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        // Get query as a list of unique tokens
        std::vector<std::string> terms = query_terms(line);
        if (terms.empty()) continue;

        // Prompt for mode. Anything other than "c" means disjunctive.
        std::cout << "mode (c = conjunctive, d = disjunctive)> " << std::flush;
        if (!std::getline(std::cin, mode)) break;
        bool conj = !mode.empty() && (mode[0] == 'c' || mode[0] == 'C');


        auto start = Clock::now();
        std::unordered_map<std::string, double> term_idf;

        // Run the query to find close matches using the index
        std::vector<Result> results = run_query(idx, terms, conj, term_idf);

        //Printing results
        std::cout << std::fixed << std::setprecision(4);
        if (results.empty()) std::cout << "no results\n";

        for (size_t i = 0; i < results.size(); i++) {
            const Result &r = results[i];
            std::cout << std::setw(2) << (i + 1) << ".  score " << r.score << "  passage "
                      << idx.page_table[r.docid] << "\n    ";
            for (size_t t = 0; t < terms.size(); t++) std::cout << terms[t] << " " << r.term_scores[t] << "  ";
            std::cout << '\n';
            if (show_text) std::cout << "    " << snippet(passage_text(idx, r.docid), term_idf, markup) << '\n';
            std::cout << '\n';
        }

        // Printing stats 
        std::cout << (conj ? "conjunctive" : "disjunctive") << ", "
                  << (seconds_since(start) * 1000) << " ms\n";
    }
    std::cout << '\n';
}

static std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                   std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Value of one parameter in a request target 
static std::string query_param(const std::string &target, const std::string &name) {
    size_t pos = target.find('?');
    while (pos != std::string::npos) {
        size_t start = pos + 1;
        pos = target.find('&', start);
        std::string pair = target.substr(start, pos == std::string::npos ? pos : pos - start);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) return url_decode(pair.substr(eq + 1));
    }
    return "";
}

// The browser gets the results as a ready-made HTML fragment, so the page needs no JSON handling.
static std::string search_html(Index &idx, const std::string &query, bool conj, bool show_text) {
    static const Markup markup = {"<b>", "</b>", true};
    auto start = Clock::now();
    std::unordered_map<std::string, double> term_idf;
    std::vector<std::string> terms = query_terms(query);
    std::vector<Result> results = run_query(idx, terms, conj, term_idf);

    // Terms are only [a-z0-9], so they go into the passage link without URL encoding.
    std::string q;
    for (auto &term : terms) q += (q.empty() ? "" : "+") + term;

    std::ostringstream out;
    out << std::fixed;
    if (results.empty()) out << "<p>no results</p>";
    out << "<ol>";
    for (auto &r : results) {
        out << "<li><div class=\"head\">";
        if (show_text) out << "<a href=\"/passage?doc=" << r.docid << "&q=" << q << "\">";
        out << "passage " << idx.page_table[r.docid] << (show_text ? "</a>" : "") << " &middot; score "
            << std::setprecision(4) << r.score << "</div><div class=\"terms\">";
        for (size_t t = 0; t < terms.size(); t++) out << "<span>" << terms[t] << " " << r.term_scores[t] << "</span>";
        out << "</div>";
        if (show_text) out << "<div>" << snippet(passage_text(idx, r.docid), term_idf, markup) << "</div>";
        out << "</li>";
    }
    out << "</ol><p class=\"meta\">" << (conj ? "conjunctive" : "disjunctive") << ", "
        << std::setprecision(2) << (seconds_since(start) * 1000) << " ms</p>";
    return out.str();
}

// Full passage as its own page, with the query terms still in bold.
static std::string passage_html(Index &idx, uint32_t docid, const std::string &query) {
    static const Markup markup = {"<b>", "</b>", true};
    std::unordered_map<std::string, double> term_idf;
    for (auto &term : query_terms(query)) term_idf[term] = 0;

    std::ostringstream out;
    out << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        << "<title>Passage " << idx.page_table[docid] << "</title>"
        << "<style>body { font-family: system-ui, sans-serif; max-width: 760px; margin: 40px auto; "
        << "padding: 0 16px; line-height: 1.6; } .meta { color: #666; font-size: 13px; }</style></head><body>"
        << "<p><a href=\"javascript:history.back()\">&larr; Back to results</a></p>"
        << "<h1>Passage " << idx.page_table[docid] << "</h1>"
        << "<p class=\"meta\">" << idx.doc_lengths[docid] << " words</p>"
        << "<p>" << snippet(passage_text(idx, docid), term_idf, markup, SIZE_MAX) << "</p>"
        << "</body></html>";
    return out.str();
}

static void send_response(int fd, const char *status, const char *type, const std::string &body) {
    std::string response = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body;
    for (size_t sent = 0; sent < response.size();) {
        ssize_t n = write(fd, response.data() + sent, response.size() - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

// Minimal single-threaded HTTP server: "/" serves web/index.html (plus its style.css), "/search" runs a query,
// "/passage" shows one full passage.
static int serve(Index &idx, int port, bool show_text) {
    signal(SIGPIPE, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // Bound to localhost only, so the index is not exposed to the network.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(server, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 16) < 0) {
        std::cerr << "cannot listen on port " << port << "\n";
        return 1;
    }
    std::cerr << "serving on http://localhost:" << port << "\n";

    while (true) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;

        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
            ssize_t n = read(client, buf, sizeof(buf));
            if (n <= 0) break;
            request.append(buf, (size_t)n);
        }

        std::istringstream first_line(request);
        std::string method, target;
        first_line >> method >> target;

        std::string path = target.substr(0, target.find('?'));

        if (path == "/" || path == "/index.html" || path == "/style.css") {
            bool css = path == "/style.css";
            std::string file = css ? "web/style.css" : "web/index.html";
            std::ifstream page(file);
            std::stringstream body;
            body << page.rdbuf();
            if (page) send_response(client, "200 OK", css ? "text/css; charset=utf-8" : "text/html; charset=utf-8", body.str());
            else send_response(client, "404 Not Found", "text/plain", file + " not found; run from the repo root\n");
        } else if (path == "/search") {
            bool conj = query_param(target, "mode") == "c";
            send_response(client, "200 OK", "text/html; charset=utf-8",
                          search_html(idx, query_param(target, "q"), conj, show_text));
        } else if (path == "/passage" && show_text) {
            std::string doc = query_param(target, "doc");
            bool valid = !doc.empty() && doc.size() <= 9 &&
                         std::all_of(doc.begin(), doc.end(), [](unsigned char c) { return std::isdigit(c); }) &&
                         std::stoul(doc) < idx.num_docs;
            if (valid) {
                send_response(client, "200 OK", "text/html; charset=utf-8",
                              passage_html(idx, (uint32_t)std::stoul(doc), query_param(target, "q")));
            } else {
                send_response(client, "404 Not Found", "text/plain", "no such passage\n");
            }
        } else {
            send_response(client, "404 Not Found", "text/plain", "not found\n");
        }
        close(client);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <runs_dir> <index_dir> [collection.tsv] [port]\n";
        return 1;
    }

    Index idx;
    auto load_start = Clock::now();
    if (!load_index(idx, argv[1], argv[2])) {
        std::cerr << "cannot load index from " << argv[1] << " and " << argv[2] << "\n";
        return 1;
    }
    // Passage text is optional: without the collection, results show passage IDs only.
    bool show_text = false;
    if (argc > 3) {
        idx.collection_in.open(argv[3], std::ios::binary);
        show_text = idx.collection_in &&
                    read_binary_file(std::string(argv[1]) + "/doc_offsets.bin", idx.doc_offsets) &&
                    idx.doc_offsets.size() == idx.num_docs;
        if (!show_text) std::cerr << "cannot read passage text, showing passage IDs only\n";
    }

    std::cerr << std::fixed << std::setprecision(2)
              << "loaded " << idx.lexicon.size() << " terms, " << idx.num_docs << " docs in "
              << seconds_since(load_start) << "s\n";

    // A port argument switches from the command-line loop to the browser interface.
    if (argc > 4) return serve(idx, std::stoi(argv[4]), show_text);
    command_line(idx, show_text);
    return 0;
}
