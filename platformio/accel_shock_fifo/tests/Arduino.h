#pragma once

#include <stdint.h>

namespace host_clock {
inline uint64_t nowUs = 0;
}

inline uint32_t millis() { return static_cast<uint32_t>(host_clock::nowUs / 1000); }
inline uint32_t micros() { return static_cast<uint32_t>(host_clock::nowUs); }
inline void delay(uint32_t ms) { host_clock::nowUs += static_cast<uint64_t>(ms) * 1000; }
