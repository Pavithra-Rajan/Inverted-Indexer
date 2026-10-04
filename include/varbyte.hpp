#pragma once
#include <cstdint>
#include <vector>


inline void varbyte_encode(uint32_t num, std::vector<uint8_t> &out) {
    while (num >= 128) {
        //take last 7 bits of num and add 128 to set the bit to indicte more bytes to come.
        out.push_back((uint8_t)(128 + (num & 127)));
        //shift num right by 7 bits to process the next 7 bits in the next iteration
        num >>= 7;
    }
    out.push_back((uint8_t)num);
}


inline uint32_t varbyte_decode(const uint8_t *&p) {
    uint32_t num = 0;
    int shift = 0;
    while (*p >= 128) {
        num += (uint32_t)(*p & 127) << shift;
        shift += 7;
        p++;
    }
    num += (uint32_t)(*p) << shift;
    p++;
    return num;
}