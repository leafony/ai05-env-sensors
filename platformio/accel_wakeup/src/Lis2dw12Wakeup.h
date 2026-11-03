#pragma once

#include <Arduino.h>
#include <Wire.h>

// Wake-up configuration: ST AN5038, sections 3.4, 4.4 and 5.4.
// https://www.st.com/resource/en/application_note/an5038-lis2dw12-alwayson-3axis-accelerometer-stmicroelectronics.pdf
// Register definitions and acceleration scaling:
// https://github.com/STMicroelectronics/lis2dw12-pid
// Wire errors are returned to the caller, including a short register read.
class Lis2dw12Wakeup {
 public:
  static constexpr uint16_t kOutputDataRateHz = 100;
  static constexpr float kThresholdStepG = 2.0f / 64.0f;
  static constexpr uint8_t kWakeupEvent = 0x08;
  static constexpr uint8_t kWakeupX = 0x04;
  static constexpr uint8_t kWakeupY = 0x02;
  static constexpr uint8_t kWakeupZ = 0x01;

  // thresholdCounts: 1..63, one count = 0.03125 g at +/-2 g.
  // durationCounts: 0..3, one count = 1/ODR = 10 ms; 0 adds no delay.
  // INT1 remains disabled until enableWakeupInterrupt() is called.
  bool begin(uint8_t address, uint8_t thresholdCounts, uint8_t durationCounts) {
    ready_ = false;
    if ((address != 0x18 && address != 0x19) ||
        thresholdCounts == 0 || thresholdCounts > 63 || durationCounts > 3) {
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

    // CTRL2: BDU and address auto-increment.
    // CTRL3: latched, active-high, push-pull interrupt.
    // CTRL6: +/-2 g, low noise, bandwidth ODR/20, FDS=0 (gravity retained
    // in output registers). CTRL7: HP-filtered wake-up, interrupts enabled.
    // CTRL1: 100 Hz, high-performance, 14-bit output.
    if (!writeRegister(kCtrl2, 0x0C) ||
        !writeRegister(kCtrl3, 0x10) ||
        !writeRegister(kInt1Route, 0x00) ||
        !writeRegister(kCtrl6, 0xC4) ||
        !writeRegister(kWakeupThreshold, thresholdCounts) ||
        !writeRegister(kWakeupDuration, static_cast<uint8_t>(durationCounts << 5)) ||
        !writeRegister(kCtrl7, 0x20) ||
        !writeRegister(kCtrl1, 0x54)) {
      return false;
    }

    // HP cutoff is 5 Hz; AN5038 specifies 11 samples for 95% settling.
    // Keep the interrupt disconnected during startup and clear old events.
    delay(200);
    uint8_t source = 0;
    if (!readRegisters(kWakeupSource, &source, 1)) {
      return false;
    }
    ready_ = true;
    return true;
  }

  // Call after attaching the MCU's RISING-edge handler to INT1.
  bool enableWakeupInterrupt() {
    uint8_t source = 0;
    return ready_ && readRegisters(kWakeupSource, &source, 1) &&
           writeRegister(kInt1Route, 0x20);
  }

  // Read only in response to a pending interrupt. Reading WAKE_UP_SRC
  // captures the event axes and clears the latched INT1 request.
  // A failed transaction leaves source unchanged so callers can retry.
  bool readWakeupSource(uint8_t &source) {
    uint8_t value = 0;
    if (!ready_ || !readRegisters(kWakeupSource, &value, 1)) {
      return false;
    }
    source = value;
    return true;
  }

  // Latest low-pass output, including gravity, in g. This is not a stored
  // snapshot of the acceleration that originally triggered the interrupt.
  bool readAcceleration(float &xG, float &yG, float &zG) {
    uint8_t data[6];
    if (!ready_ || !readRegisters(kOutXL, data, sizeof(data))) {
      return false;
    }
    // +/-2 g, 14-bit left-aligned output: 0.244 mg / 4 per raw LSB.
    xG = decodeAxis(data) * 0.000061f;
    yG = decodeAxis(data + 2) * 0.000061f;
    zG = decodeAxis(data + 4) * 0.000061f;
    return true;
  }

 private:
  static constexpr uint8_t kWhoAmI = 0x0F;
  static constexpr uint8_t kCtrl1 = 0x20;
  static constexpr uint8_t kCtrl2 = 0x21;
  static constexpr uint8_t kCtrl3 = 0x22;
  static constexpr uint8_t kInt1Route = 0x23;
  static constexpr uint8_t kCtrl6 = 0x25;
  static constexpr uint8_t kOutXL = 0x28;
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
