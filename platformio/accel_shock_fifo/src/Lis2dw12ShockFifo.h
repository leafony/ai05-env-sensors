#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "ShockCapture.h"

// ST LIS2DW12 DS11811 section 5.3 and AN5038 sections 5.4, 6.4.5, 6.5.
// https://www.st.com/resource/en/datasheet/lis2dw12.pdf
// https://www.st.com/resource/en/application_note/an5038-lis2dw12-alwayson-3axis-accelerometer-stmicroelectronics.pdf
class Lis2dw12ShockFifo {
 public:
  static constexpr uint16_t kOutputDataRateHz = ShockCapture::kOutputDataRateHz;
  static constexpr uint8_t kRangeG = 8;
  static constexpr float kThresholdStepG = 8.0f / 64.0f;
  // 14-bit +/-8 g output is left-aligned in a 16-bit word: 0.976 mg / 4.
  static constexpr float kRawToG = 0.000244f;
  static constexpr uint8_t kWakeupEvent = 0x08;

  bool begin(uint8_t address, uint8_t thresholdCounts) {
    ready_ = false;
    if ((address != 0x18 && address != 0x19) || thresholdCounts == 0 ||
        thresholdCounts > 63) {
      return false;
    }
    address_ = address;
    uint8_t id = 0;
    if (!readRegisters(kWhoAmI, &id, 1) || id != 0x44 ||
        !writeRegister(kCtrl2, 0x40)) {
      return false;
    }
    const uint32_t resetStarted = millis();
    for (;;) {
      uint8_t ctrl2 = 0;
      if (!readRegisters(kCtrl2, &ctrl2, 1)) {
        return false;
      }
      if ((ctrl2 & 0x40) == 0) {
        break;
      }
      if (static_cast<uint32_t>(millis() - resetStarted) >= 100) {
        return false;
      }
      delay(1);
    }

    // BDU + auto-increment; latched active-high push-pull INT1.
    // CTRL6: +/-8 g, low noise, FDS=0 (gravity retained), LPF1 ODR/2.
    // Wake-up uses the HP path (100 Hz cutoff at this configuration).
    // CTRL1: 400 Hz, high-performance. FIFO/INT1 remain disabled for settling.
    if (!writeRegister(kCtrl2, 0x0C) ||
        !writeRegister(kCtrl3, 0x10) ||
        !writeRegister(kInt1Route, 0x00) ||
        !writeRegister(kInt2Route, 0x00) ||
        !writeRegister(kFifoCtrl, 0x00) ||
        !writeRegister(kCtrl6, 0x24) ||
        !writeRegister(kWakeupThreshold, thresholdCounts) ||
        !writeRegister(kWakeupDuration, 0x00) ||
        !writeRegister(kCtrl7, 0x20) ||
        !writeRegister(kCtrl1, 0x74)) {
      return false;
    }
    delay(100);
    ready_ = true;
    uint8_t ignored = 0;
    return stopAndClear(ignored);
  }

  // Clear the FIFO and wake-up latch before disabling the route. Call only
  // outside a capture: reading WAKE_UP_SRC stops bypass-to-continuous mode!
  bool stopAndClear(uint8_t &source) {
    if (!ready_) {
      return false;
    }
    // Attempt all cleanup writes even if an earlier transaction fails.
    const bool fifoOk = writeRegister(kFifoCtrl, 0x00);
    uint8_t value = 0;
    const bool sourceOk = readRegisters(kWakeupSource, &value, 1);
    // Latching only works while the event is routed to an interrupt pin.
    // Preserve routing until after reading WU_IA, otherwise it may be lost.
    const bool routeOk = writeRegister(kInt1Route, 0x00);
    if (sourceOk) {
      source = value;
    }
    return routeOk && fifoOk && sourceOk;
  }

  // Call with the MCU handler already attached and pending flags cleared.
  bool arm() {
    if (!ready_) {
      return false;
    }
    // Clear any events accumulated during serial output/cooldown, then allow
    // a full ODR cycle for the latch reset to propagate (AN5038 section 5.4).
    uint8_t ignored = 0;
    if (!stopAndClear(ignored)) {
      return false;
    }
    delay(3);
    // Internal wake-up event starts FIFO at the next ODR, not MCU I2C latency.
    return writeRegister(kFifoCtrl, 0x80) && writeRegister(kInt1Route, 0x20);
  }

  bool readFifoStatus(FifoStatus &status) {
    uint8_t value = 0;
    if (!ready_ || !readRegisters(kFifoSamples, &value, 1)) {
      return false;
    }
    status.count = value & 0x3F;
    status.overrun = (value & 0x40) != 0;
    return true;
  }

  bool readRawSample(FifoSample &sample) {
    uint8_t data[6];
    if (!ready_ || !readRegisters(kOutXL, data, sizeof(data))) {
      return false;
    }
    sample.x = decodeAxis(data);
    sample.y = decodeAxis(data + 2);
    sample.z = decodeAxis(data + 4);
    return true;
  }

 private:
  static constexpr uint8_t kWhoAmI = 0x0F;
  static constexpr uint8_t kCtrl1 = 0x20;
  static constexpr uint8_t kCtrl2 = 0x21;
  static constexpr uint8_t kCtrl3 = 0x22;
  static constexpr uint8_t kInt1Route = 0x23;
  static constexpr uint8_t kInt2Route = 0x24;
  static constexpr uint8_t kCtrl6 = 0x25;
  static constexpr uint8_t kOutXL = 0x28;
  static constexpr uint8_t kFifoCtrl = 0x2E;
  static constexpr uint8_t kFifoSamples = 0x2F;
  static constexpr uint8_t kWakeupThreshold = 0x34;
  static constexpr uint8_t kWakeupDuration = 0x35;
  static constexpr uint8_t kWakeupSource = 0x38;
  static constexpr uint8_t kCtrl7 = 0x3F;

  static int16_t decodeAxis(const uint8_t *data) {
    return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                (static_cast<uint16_t>(data[1]) << 8));
  }

  bool writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
  }

  bool readRegisters(uint8_t reg, uint8_t *data, uint8_t length) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom(address_, length) != length) {
      return false;
    }
    for (uint8_t i = 0; i < length; ++i) {
      const int value = Wire.read();
      if (value < 0) {
        return false;
      }
      data[i] = static_cast<uint8_t>(value);
    }
    return true;
  }

  uint8_t address_ = 0;
  bool ready_ = false;
};
