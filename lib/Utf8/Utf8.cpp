#include "Utf8.h"
#include <cstdint>
#include <cstring>  

static bool isUtf8Trail(unsigned char c);
int utf8CodepointLen(const unsigned char c);

void calibrateUtf8Pointer(const unsigned char* text) {
    if (text == nullptr) return;
    const unsigned char* ptr = text;
    while (*ptr != '\0') {
        utf8NextCodepoint(&ptr);
    }
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
    if (string == nullptr || *string == nullptr) {
        return 0xFFFD;
    }

    const unsigned char* original = *string;
    const int bytes = utf8CodepointLen(*original);

    if (bytes == 0) {
        (*string)++;
        return 0xFFFD;
    }

    for (int i = 1; i < bytes; ++i) {
        if (!isUtf8Trail(original[i])) {
            (*string) += i;
            return 0xFFFD;
        }
    }

    uint32_t cp = 0;
    switch (bytes) {
        case 1: 
            cp = original[0]; 
            break;
        case 2: 
            cp = ((original[0] & 0x1F) << 6) | (original[1] & 0x3F); 
            break;
        case 3: 
            if (*original == 0xE2 && *(original+1) == 0x80 && *(original+2) == 0xA6) {
                cp = 0x2026;
            } else if (*original == 0xE3 && *(original+1) == 0x80 && *(original+2) == 0x82) {
                cp = 0x3002;
            } else {
                cp = ((original[0] & 0x0F) << 12) | ((original[1] & 0x3F) << 6) | (original[2] & 0x3F);
            }
            break;
        case 4: 
            cp = ((original[0] & 0x07) << 18) | ((original[1] & 0x3F) << 12) | ((original[2] & 0x3F) << 6) | (original[3] & 0x3F); 
            break;
        default: 
            cp = 0xFFFD;
            break;
    }

    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }

    *string = original + bytes;
    return cp;
}

static bool isUtf8Trail(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

int utf8CodepointLen(const unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}