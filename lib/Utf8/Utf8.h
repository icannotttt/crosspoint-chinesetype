#pragma once

#include <cstdint>
#include <Arduino.h>

void calibrateUtf8Pointer(const unsigned char* text);
uint32_t utf8NextCodepoint(const unsigned char** string);