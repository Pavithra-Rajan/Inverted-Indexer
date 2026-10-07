#pragma once
#include <string>
#include <vector>
#include <cctype>


// Gets a text from caller which it checks is a ASCII byte and alpanumeric. 
// If it passes this check it is made lower case and added to buffer. 
// When non-ASCII byte, whitespace or non-alphanumeric char is encountered,
// the characters in buffer is considered a word and moved into output.
inline void tokenize(const std::string &text, std::vector<std::string> &out) {
    out.clear();
    std::string tok;
    for (unsigned char c : text) {
        if (c < 128 && std::isalnum(c)) {
            tok += (char)std::tolower(c);
        } else if (!tok.empty()) {
            out.push_back(std::move(tok));
            tok.clear();
        }
    }
    if (!tok.empty()) out.push_back(std::move(tok));
}

struct TokenSpan {
    std::string term;
    size_t start, end;
};

// Same rules as tokenize, but keeps each token's byte range in the text (used for snippets).
inline void tokenize_spans(const std::string &text, std::vector<TokenSpan> &out) {
    out.clear();
    std::string tok;
    for (size_t i = 0; i <= text.size(); i++) {
        unsigned char c = i < text.size() ? (unsigned char)text[i] : ' ';
        if (c < 128 && std::isalnum(c)) {
            tok += (char)std::tolower(c);
        } else if (!tok.empty()) {
            out.push_back({tok, i - tok.size(), i});
            tok.clear();
        }
    }
}
