#include "varbyte.hpp"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Debugging tool for bin to ascii

// Per-block metadata as written by merge: last docID and compressed sizes.
struct BlockMeta {
    uint32_t last_docid, docid_bytes, freq_bytes;
};

// Scans lexicon.txt for the term and returns its offset and df through the out params.
static bool find_term(const std::string &lexicon_path, const std::string &term,
                       uint64_t &offset, uint32_t &df) {
    std::ifstream lexicon(lexicon_path);
    std::string t;
    while (lexicon >> t >> offset >> df) {
        if (t == term) return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <index_dir> <term>\n";
        return 1;
    }
    std::string index_dir = argv[1];
    std::string term = argv[2];

    std::ios::sync_with_stdio(false);

    uint64_t offset;
    uint32_t lexicon_df;
    if (!find_term(index_dir + "/lexicon.txt", term, offset, lexicon_df)) {
        std::cerr << "term not found in lexicon: " << term << "\n";
        return 1;
    }

    std::ifstream index_in(index_dir + "/index.bin", std::ios::binary);
    if (!index_in) {
        std::cerr << "cannot open " << index_dir << "/index.bin\n";
        return 1;
    }

    // Seek to the start of this term's list and read its header: df, num_blocks
    index_in.seekg((std::streamoff)offset);
    uint32_t df, num_blocks;
    index_in.read((char *)&df, sizeof(df));
    index_in.read((char *)&num_blocks, sizeof(num_blocks));

    // Read the metadata of every block: last_docid, docid_bytes, freq_bytes
    std::vector<BlockMeta> blocks(num_blocks);
    index_in.read((char *)blocks.data(), (std::streamsize)(num_blocks * sizeof(BlockMeta)));

    // All docID blocks come first, followed by all frequency blocks.
    uint64_t total_docid_bytes = 0, total_freq_bytes = 0;
    for (auto &b : blocks) {
        total_docid_bytes += b.docid_bytes;
        total_freq_bytes += b.freq_bytes;
    }
    std::vector<uint8_t> docid_data(total_docid_bytes), freq_data(total_freq_bytes);
    index_in.read((char *)docid_data.data(), (std::streamsize)total_docid_bytes);
    index_in.read((char *)freq_data.data(), (std::streamsize)total_freq_bytes);

    std::cout << "# term=" << term << " offset=" << offset << " df=" << df
              << " num_blocks=" << num_blocks << " docid_bytes=" << total_docid_bytes
              << " freq_bytes=" << total_freq_bytes << '\n';

    const uint8_t *docid_ptr = docid_data.data();
    const uint8_t *freq_ptr = freq_data.data();
    for (uint32_t b = 0; b < num_blocks; b++) {
        std::cout << "# block " << b << " last_docid=" << blocks[b].last_docid
                  << " docid_bytes=" << blocks[b].docid_bytes
                  << " freq_bytes=" << blocks[b].freq_bytes << '\n';

        // Gaps restart from 0 in every block, so the first value is the docID itself.
        const uint8_t *block_end = docid_ptr + blocks[b].docid_bytes;
        uint32_t docid = 0;
        while (docid_ptr < block_end) {
            docid += varbyte_decode(docid_ptr);
            uint32_t freq = varbyte_decode(freq_ptr);
            std::cout << docid << '\t' << freq << '\n';
        }
    }
    return 0;
}
