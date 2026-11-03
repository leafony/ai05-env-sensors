#pragma once

#include <array>
#include <deque>
#include <set>
#include <stdint.h>
#include <utility>
#include <vector>

// A register-level I2C stub. These tests verify bus error handling and register
// access ordering; they do not simulate the MEMS/filter/FIFO timing in silicon.
class TestWire {
 public:
  std::array<uint8_t, 256> registers{};
  std::vector<std::pair<uint8_t, uint8_t>> writes;
  std::vector<uint8_t> reads;
  std::vector<std::pair<char, uint8_t>> trace;
  std::set<uint8_t> failWrites;
  std::set<uint8_t> failAddresses;
  bool shortRead = false;
  bool negativeRead = false;
  bool resetStuck = false;
  uint8_t lastAddress = 0;

  TestWire() { registers[0x0F] = 0x44; }

  void beginTransmission(uint8_t address) {
    lastAddress = address;
    tx_.clear();
  }

  void write(uint8_t value) { tx_.push_back(value); }

  uint8_t endTransmission(bool = true) {
    if (tx_.empty()) {
      return 4;
    }
    cursor_ = tx_[0];
    if (tx_.size() == 1) {
      return failAddresses.count(cursor_) ? 2 : 0;
    }
    writes.emplace_back(cursor_, tx_[1]);
    trace.emplace_back('W', cursor_);
    if (failWrites.count(cursor_)) {
      return 2;
    }
    registers[cursor_] = tx_[1];
    if (cursor_ == 0x21 && tx_[1] == 0x40 && !resetStuck) {
      registers[cursor_] = 0;
    }
    return 0;
  }

  uint8_t requestFrom(uint8_t address, uint8_t length) {
    lastAddress = address;
    reads.push_back(cursor_);
    trace.emplace_back('R', cursor_);
    rx_.clear();
    const uint8_t received = shortRead && length ? length - 1 : length;
    for (uint8_t i = 0; i < received; ++i) {
      rx_.push_back(registers[static_cast<uint8_t>(cursor_ + i)]);
    }
    return received;
  }

  int read() {
    if (negativeRead || rx_.empty()) {
      return -1;
    }
    const auto value = rx_.front();
    rx_.pop_front();
    return value;
  }

 private:
  uint8_t cursor_ = 0;
  std::vector<uint8_t> tx_;
  std::deque<uint8_t> rx_;
};

inline TestWire Wire;
