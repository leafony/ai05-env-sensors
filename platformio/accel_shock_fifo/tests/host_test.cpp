#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Lis2dw12ShockFifo.h"
#include "ShockCapture.h"

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct FakeSensor {
  std::deque<FifoSample> fifo;
  int nextSample = 0;
  int rawCalls = 0;
  int statusCalls = 0;
  int failRawCall = -1;
  int failStatusCall = -1;
  int overrunStatusCall = -1;
  int invalidCountStatusCall = -1;
  int delayedStatusCall = -1;
  int delayedRawCall = -1;
  uint32_t statusDelayUs = 0;
  uint32_t rawDelayUs = 0;

  void add(int count) {
    for (int i = 0; i < count; ++i) {
      fifo.push_back({static_cast<int16_t>(nextSample),
                      static_cast<int16_t>(nextSample + 1000),
                      static_cast<int16_t>(-nextSample)});
      ++nextSample;
    }
  }

  bool readFifoStatus(FifoStatus &status) {
    ++statusCalls;
    if (statusCalls == delayedStatusCall) {
      host_clock::nowUs += statusDelayUs;
    }
    if (statusCalls == failStatusCall) {
      return false;
    }
    status.count = statusCalls == invalidCountStatusCall
                       ? 33
                       : static_cast<uint8_t>(fifo.size());
    status.overrun = statusCalls == overrunStatusCall;
    return true;
  }

  bool readRawSample(FifoSample &sample) {
    ++rawCalls;
    if (rawCalls == delayedRawCall) {
      host_clock::nowUs += rawDelayUs;
    }
    if (rawCalls == failRawCall) {
      return false;
    }
    require(!fifo.empty(), "must never read an empty FIFO");
    sample = fifo.front();
    fifo.pop_front();
    return true;
  }
};

void expectFailure(const ShockCapture &capture, const char *reason) {
  require(capture.state() == ShockCapture::State::Failed, "capture must fail");
  require(std::strcmp(capture.error(), reason) == 0, "failure reason mismatch");
}

void acquisitionExactly100() {
  FakeSensor sensor;
  ShockCapture capture;
  capture.start(100);
  for (int i = 0; i < 4; ++i) {
    sensor.add(31);
    capture.poll(sensor, 110 + i * 80, [] { return uint32_t{0}; });
  }
  require(capture.state() == ShockCapture::State::Complete, "capture must complete");
  require(capture.count() == 100, "capture must contain exactly 100 samples");
  require(sensor.rawCalls == 101, "discard exactly one initial sample");
  require(sensor.fifo.size() == 23, "do not over-read the final batch");
  for (int i = 0; i < 100; ++i) {
    const auto &sample = capture.samples()[i];
    require(sample.x == i + 1 && sample.y == i + 1001 && sample.z == -(i + 1),
            "retained samples must remain ordered and contiguous");
  }
  const int calls = sensor.statusCalls;
  capture.poll(sensor, 2000, [] { return uint32_t{0}; });
  require(sensor.statusCalls == calls, "complete capture must not touch sensor");
}

void varyingSmallBatches() {
  FakeSensor sensor;
  ShockCapture capture;
  capture.start(0);
  const int sizes[] = {0, 1, 3, 7, 19, 2, 15, 11, 0, 17, 26};
  uint32_t now = 0;
  for (int count : sizes) {
    sensor.add(count);
    capture.poll(sensor, now += 30, [] { return uint32_t{0}; });
  }
  require(capture.state() == ShockCapture::State::Complete, "small batches must complete");
  require(sensor.rawCalls == 101 && capture.count() == 100, "small batch count mismatch");
  require(capture.samples()[99].x == 100, "first sample discarded across batches");
}

void idleAndEmpty() {
  ShockCapture capture;
  FakeSensor sensor;
  capture.poll(sensor, 0, [] { return uint32_t{0}; });
  require(sensor.statusCalls == 0 && sensor.rawCalls == 0, "idle must do no I2C");
  capture.start(0);
  capture.poll(sensor, 5, [] { return uint32_t{0}; });
  require(sensor.rawCalls == 0, "empty FIFO must not be read");
  sensor.add(1);
  capture.poll(sensor, 10, [] { return uint32_t{0}; });
  require(capture.count() == 0, "first arriving sample must still be discarded");
  sensor.add(1);
  capture.poll(sensor, 12, [] { return uint32_t{0}; });
  require(capture.count() == 1 && capture.samples()[0].x == 1, "second sample must be retained");
}

void statusReadFailures() {
  for (int failCall : {1, 2}) {
    ShockCapture capture;
    FakeSensor sensor;
    sensor.add(4);
    sensor.failStatusCall = failCall;
    capture.start(0);
    capture.poll(sensor, 1, [] { return uint32_t{0}; });
    expectFailure(capture, "fifo_status_read");
    const int calls = sensor.statusCalls;
    capture.poll(sensor, 2, [] { return uint32_t{0}; });
    require(sensor.statusCalls == calls, "failure must remain terminal until restart");
  }
}

void sampleReadFailures() {
  for (int failCall : {1, 3}) {
    ShockCapture capture;
    FakeSensor sensor;
    sensor.add(4);
    sensor.failRawCall = failCall;
    capture.start(0);
    capture.poll(sensor, 1, [] { return uint32_t{0}; });
    expectFailure(capture, "fifo_sample_read");
    require(capture.count() == (failCall == 1 ? 0 : 1), "partial capture count wrong");
  }
}

void overrunAndInvalidCount() {
  for (int call : {1, 2}) {
    for (bool overrun : {false, true}) {
      ShockCapture capture;
      FakeSensor sensor;
      sensor.add(4);
      if (overrun) {
        sensor.overrunStatusCall = call;
      } else {
        sensor.invalidCountStatusCall = call;
      }
      capture.start(0);
      capture.poll(sensor, 1, [] { return uint32_t{0}; });
      expectFailure(capture, overrun ? "fifo_overrun" : "invalid_fifo_count");
      if (call == 1) {
        require(sensor.rawCalls == 0, "invalid snapshot must be rejected before any pop");
      }
    }
  }
}

void finalStatusFailureRejects100() {
  for (bool overrun : {false, true}) {
    ShockCapture capture;
    FakeSensor sensor;
    capture.start(0);
    for (int i = 0; i < 3; ++i) {
      sensor.add(31);
      capture.poll(sensor, 10 + i * 80, [] { return uint32_t{0}; });
    }
    sensor.add(8);
    if (overrun) {
      sensor.overrunStatusCall = sensor.statusCalls + 2;
    } else {
      sensor.failStatusCall = sensor.statusCalls + 2;
    }
    capture.poll(sensor, 250, [] { return uint32_t{0}; });
    require(capture.count() == 100, "test must reach 100 before final status check");
    expectFailure(capture, overrun ? "fifo_overrun" : "fifo_status_read");
  }
}

void timeoutAndMillisWrap() {
  ShockCapture capture;
  FakeSensor sensor;
  const uint32_t trigger = UINT32_MAX - 10;
  capture.start(trigger);
  capture.poll(sensor, 5, [] { return uint32_t{0}; });
  require(capture.state() == ShockCapture::State::Collecting, "millis wrap must not cause early timeout");
  capture.poll(sensor, trigger + ShockCapture::kTimeoutMs - 1, [] { return uint32_t{0}; });
  require(capture.state() == ShockCapture::State::Collecting, "timeout boundary too early");
  const int calls = sensor.statusCalls;
  capture.poll(sensor, trigger + ShockCapture::kTimeoutMs, [] { return uint32_t{0}; });
  expectFailure(capture, "capture_timeout");
  require(sensor.statusCalls == calls, "timeout should be checked before bus access");
}

void restartAfterFailure() {
  ShockCapture capture;
  FakeSensor sensor;
  sensor.overrunStatusCall = 1;
  capture.start(1);
  capture.poll(sensor, 2, [] { return uint32_t{0}; });
  expectFailure(capture, "fifo_overrun");
  sensor = FakeSensor{};
  sensor.add(2);
  capture.start(300);
  capture.poll(sensor, 301, [] { return uint32_t{0}; });
  require(capture.state() == ShockCapture::State::Collecting, "restart should collect");
  require(capture.triggerMs() == 300 && capture.count() == 1, "restart metadata mismatch");
  require(std::strcmp(capture.error(), "none") == 0 && capture.samples()[0].x == 1,
          "restart should reset error and first-sample discard");
}

void serviceDeadlines() {
  // With 31 unread entries, the conservative service margin is one 2 ms slot.
  // Delays before the first pop, during a pop, or after the batch all invalidate
  // it. During-pop coverage matters because popping clears the chip's OVR flag.
  for (int phase = 0; phase < 3; ++phase) {
    FakeSensor sensor;
    ShockCapture capture;
    sensor.add(31);
    host_clock::nowUs = 0;
    if (phase == 0) {
      sensor.delayedStatusCall = 1;
      sensor.statusDelayUs = 2000;
    } else if (phase == 1) {
      sensor.delayedRawCall = 1;
      sensor.rawDelayUs = 2000;
    } else {
      sensor.delayedStatusCall = 2;
      sensor.statusDelayUs = 2000;
    }
    capture.start(0);
    capture.poll(sensor, 1, micros);
    expectFailure(capture, "fifo_service_delayed");
    if (phase == 0) require(sensor.rawCalls == 0, "late status must be rejected before a pop");
    if (phase == 1) require(capture.count() == 0, "late pop must not become a retained sample");
  }
  for (int initialCount : {0, 32}) {
    FakeSensor sensor;
    ShockCapture capture;
    sensor.add(initialCount);
    host_clock::nowUs = 0;
    if (initialCount == 0) {
      sensor.delayedStatusCall = 1;
      sensor.statusDelayUs = 64000;
    }
    capture.start(0);
    capture.poll(sensor, 1, micros);
    expectFailure(capture, "fifo_service_delayed");
    require(sensor.rawCalls == 0, "full or excessively delayed snapshot must not be popped");
  }
}

void serviceDeadlineMicrosWrap() {
  for (uint32_t delayUs : {1999U, 2000U}) {
    FakeSensor sensor;
    ShockCapture capture;
    sensor.add(31);
    host_clock::nowUs = UINT32_MAX - 1000;
    sensor.delayedRawCall = 1;
    sensor.rawDelayUs = delayUs;
    capture.start(0);
    capture.poll(sensor, 1, micros);
    if (delayUs == 1999) {
      require(capture.state() == ShockCapture::State::Collecting && capture.count() == 30,
              "micros wrap must preserve valid service just below deadline");
    } else {
      expectFailure(capture, "fifo_service_delayed");
    }
  }
}

void resetBus() {
  Wire = TestWire{};
  host_clock::nowUs = 0;
}

void driverInitializationAndArm() {
  resetBus();
  Lis2dw12ShockFifo sensor;
  require(sensor.begin(0x18, 4), "driver must initialize");
  require(Wire.registers[0x20] == 0x74 && Wire.registers[0x21] == 0x0C &&
              Wire.registers[0x22] == 0x10 && Wire.registers[0x25] == 0x24 &&
              Wire.registers[0x34] == 4 && Wire.registers[0x35] == 0 &&
              Wire.registers[0x3F] == 0x20,
          "ODR/range/HP/latch configuration mismatch");
  require(Wire.registers[0x23] == 0 && Wire.registers[0x2E] == 0,
          "initialization must leave FIFO/IRQ disconnected");
  Wire.writes.clear();
  Wire.trace.clear();
  const auto startMs = millis();
  require(sensor.arm(), "arm must succeed");
  require(millis() - startMs >= 3, "arm must wait for latch clear propagation");
  require(Wire.writes.size() == 4 && Wire.writes[0] == std::make_pair(uint8_t{0x2E}, uint8_t{0}) &&
              Wire.writes[1] == std::make_pair(uint8_t{0x23}, uint8_t{0}) &&
              Wire.writes[2] == std::make_pair(uint8_t{0x2E}, uint8_t{0x80}) &&
              Wire.writes[3] == std::make_pair(uint8_t{0x23}, uint8_t{0x20}),
          "arm must clear FIFO before configuring hardware trigger and route last");
  require(Wire.trace.size() == 5 && Wire.trace[1] == std::make_pair('R', uint8_t{0x38}) &&
              Wire.trace[2] == std::make_pair('W', uint8_t{0x23}),
          "capture source must be read before interrupt routing is disabled");
}

void driverRawDecodeAndLatchPreservation() {
  resetBus();
  Lis2dw12ShockFifo sensor;
  require(sensor.begin(0x19, 4) && sensor.arm(), "alternate address must work");
  require(Wire.lastAddress == 0x19, "wrong I2C address");
  Wire.reads.clear();
  Wire.registers[0x2F] = 0x60;
  FifoStatus status{};
  require(sensor.readFifoStatus(status) && status.count == 32 && status.overrun,
          "32-deep count and overrun decoding mismatch");
  Wire.registers[0x28] = 0xFC;
  Wire.registers[0x29] = 0x7F;
  Wire.registers[0x2A] = 0x00;
  Wire.registers[0x2B] = 0x80;
  Wire.registers[0x2C] = 0xFC;
  Wire.registers[0x2D] = 0xFF;
  FifoSample sample{};
  require(sensor.readRawSample(sample), "raw sample read failed");
  require(sample.x == 32764 && sample.y == -32768 && sample.z == -4,
          "signed little-endian axis decode mismatch");
  require(std::fabs(sample.y * sensor.kRawToG + 7.995392f) < 0.000001f,
          "+/-8 g scaling mismatch");
  require(std::find(Wire.reads.begin(), Wire.reads.end(), uint8_t{0x38}) == Wire.reads.end() &&
              std::find(Wire.reads.begin(), Wire.reads.end(), uint8_t{0x3B}) == Wire.reads.end(),
          "data/status reads must never clear wake-up latch");
}

void driverRejectsFailedReadsWithoutMutatingOutput() {
  for (int failure = 0; failure < 3; ++failure) {
    resetBus();
    Lis2dw12ShockFifo sensor;
    require(sensor.begin(0x18, 4), "driver must initialize");
    if (failure == 0) Wire.shortRead = true;
    if (failure == 1) Wire.negativeRead = true;
    if (failure == 2) Wire.failAddresses = {0x28, 0x2F};
    FifoSample sample{11, 22, 33};
    require(!sensor.readRawSample(sample), "I2C failure must be returned");
    require(sample.x == 11 && sample.y == 22 && sample.z == 33, "partial reads must not mutate sample");
    FifoStatus status{7, true};
    require(!sensor.readFifoStatus(status), "status read failure must be returned");
    require(status.count == 7 && status.overrun, "failed read must not mutate status");
  }
}

void driverInitFailuresAndCleanup() {
  resetBus();
  Lis2dw12ShockFifo sensor;
  require(!sensor.begin(0x1A, 4) && !sensor.begin(0x18, 0) && !sensor.begin(0x18, 64),
          "invalid configuration must fail");
  Wire.registers[0x0F] = 0x33;
  require(!sensor.begin(0x18, 4), "wrong sensor ID must fail");
  resetBus();
  Wire.resetStuck = true;
  require(!sensor.begin(0x18, 4) && millis() >= 100, "reset must have a bounded timeout");
  resetBus();
  require(sensor.begin(0x18, 4), "driver must recover after fresh initialization");
  Wire.writes.clear();
  Wire.failWrites.insert(0x23);
  Wire.registers[0x38] = 0x0C;
  uint8_t source = 0;
  require(!sensor.stopAndClear(source), "cleanup must report failed route write");
  require(Wire.writes.size() == 2 && Wire.writes[0].first == 0x2E && source == 0x0C,
          "cleanup must still attempt FIFO reset and source read");
}
}  // namespace

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"exactly 100 ordered samples and no final over-read", acquisitionExactly100},
      {"varying small batches", varyingSmallBatches},
      {"idle and empty FIFO", idleAndEmpty},
      {"status transaction failures", statusReadFailures},
      {"sample transaction failures", sampleReadFailures},
      {"overrun and invalid FIFO counts", overrunAndInvalidCount},
      {"final status failure rejects an otherwise full capture", finalStatusFailureRejects100},
      {"timeout including millis wrap", timeoutAndMillisWrap},
      {"restart after failure", restartAfterFailure},
      {"status/pop/final service deadlines and full FIFO rejection", serviceDeadlines},
      {"service deadline including micros wrap", serviceDeadlineMicrosWrap},
      {"driver initialization and rearm ordering", driverInitializationAndArm},
      {"driver signed decoding and latch preservation", driverRawDecodeAndLatchPreservation},
      {"driver short read, negative read and NACK", driverRejectsFailedReadsWithoutMutatingOutput},
      {"driver invalid configuration, reset timeout and cleanup", driverInitFailuresAndCleanup},
  };
  int failed = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << (sizeof(tests) / sizeof(tests[0]) - failed) << '/' << sizeof(tests) / sizeof(tests[0])
            << " host checks passed\n";
  return failed ? 1 : 0;
}
