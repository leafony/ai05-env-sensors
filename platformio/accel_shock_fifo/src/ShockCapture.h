#pragma once

#include <stdint.h>

struct FifoSample {
  int16_t x;
  int16_t y;
  int16_t z;
};

struct FifoStatus {
  uint8_t count;
  bool overrun;
};

// Platform-independent acquisition state, shared by the firmware and host tests.
// A Sensor supplies readFifoStatus(FifoStatus&) and readRawSample(FifoSample&).
class ShockCapture {
 public:
  static constexpr uint16_t kSampleCount = 100;
  static constexpr uint16_t kOutputDataRateHz = 400;
  // 20% below the nominal 2500 us period; used only as a conservative deadline.
  static constexpr uint32_t kMinSamplePeriodUs = 2000;
  static constexpr uint32_t kTimeoutMs = 1000;

  enum class State { Idle, Collecting, Complete, Failed };

  void start(uint32_t triggerMs) {
    state_ = State::Collecting;
    count_ = 0;
    discardFirst_ = true;
    triggerMs_ = triggerMs;
    error_ = nullptr;
  }

  template <typename Sensor, typename Clock>
  void poll(Sensor &sensor, uint32_t nowMs, Clock clockUs) {
    if (state_ != State::Collecting) {
      return;
    }
    if (static_cast<uint32_t>(nowMs - triggerMs_) >= kTimeoutMs) {
      fail("capture_timeout");
      return;
    }

    const uint32_t serviceStartedUs = clockUs();
    FifoStatus status{};
    if (!sensor.readFifoStatus(status)) {
      fail("fifo_status_read");
      return;
    }
    if (!checkStatus(status)) {
      return;
    }
    // A pop clears FIFO_OVR, so flags alone could miss an overrun during a
    // delayed I2C read. Ignore slots freed while draining and conservatively
    // reject the batch if even the original free slots could have filled.
    // A completely full snapshot has no timing margin and is also rejected.
    const uint32_t serviceBudgetUs = (32 - status.count) * kMinSamplePeriodUs;
    if (static_cast<uint32_t>(clockUs() - serviceStartedUs) >= serviceBudgetUs) {
      fail("fifo_service_delayed");
      return;
    }

    // Read no more than this snapshot's unread count. Never read an empty FIFO:
    // LIS2DW12 would return the last sample again, fabricating extra samples.
    for (uint8_t i = 0; i < status.count && count_ < kSampleCount; ++i) {
      FifoSample sample{};
      if (!sensor.readRawSample(sample)) {
        // A partial read may have advanced the hardware FIFO. Do not retry it
        // as though the capture were still contiguous.
        fail("fifo_sample_read");
        return;
      }
      if (static_cast<uint32_t>(clockUs() - serviceStartedUs) >= serviceBudgetUs) {
        fail("fifo_service_delayed");
        return;
      }
      if (discardFirst_) {
        // Datasheet section 5.3: discard the first sample after entering FIFO.
        discardFirst_ = false;
      } else {
        samples_[count_++] = sample;
      }
    }

    // Check both flags and the time budget, including the final batch.
    FifoStatus after{};
    if (!sensor.readFifoStatus(after)) {
      fail("fifo_status_read");
      return;
    }
    if (!checkStatus(after)) {
      return;
    }
    if (static_cast<uint32_t>(clockUs() - serviceStartedUs) >= serviceBudgetUs) {
      fail("fifo_service_delayed");
      return;
    }
    if (count_ == kSampleCount) {
      state_ = State::Complete;
    }
  }

  State state() const { return state_; }
  uint16_t count() const { return count_; }
  uint32_t triggerMs() const { return triggerMs_; }
  const FifoSample *samples() const { return samples_; }
  const char *error() const { return error_ == nullptr ? "none" : error_; }

 private:
  void fail(const char *error) {
    error_ = error;
    state_ = State::Failed;
  }

  bool checkStatus(const FifoStatus &status) {
    if (status.overrun) {
      fail("fifo_overrun");
      return false;
    }
    if (status.count > 32) {
      fail("invalid_fifo_count");
      return false;
    }
    return true;
  }

  State state_ = State::Idle;
  FifoSample samples_[kSampleCount]{};
  uint16_t count_ = 0;
  uint32_t triggerMs_ = 0;
  bool discardFirst_ = true;
  const char *error_ = nullptr;
};
