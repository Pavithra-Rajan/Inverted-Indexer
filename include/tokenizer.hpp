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
