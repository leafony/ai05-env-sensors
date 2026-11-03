#pragma once

#include <Arduino.h>
#include <Wire.h>

// Register definitions and conversion follow ST's official driver:
// https://github.com/stm32duino/LIS2DW12/blob/main/src/lis2dw12_reg.h
// https://github.com/stm32duino/LIS2DW12/blob/main/src/lis2dw12_reg.c
// Use Wire directly so NACKs and short reads propagate to the caller.
class Lis2dw12 {
 public:
  bool begin(uint8_t address) {
    ready_ = false;
    address_ = address;

    uint8_t id = 0;
    if (!readRegisters(kWhoAmI, &id, 1) || id != 0x44) {
      return false;
    }

    // Restore known defaults, with a bounded wait for SOFT_RESET to clear.
    if (!writeRegister(kCtrl2, 0x40)) {
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

    // CTRL2: BDU + IF_ADD_INC; CTRL6: +/-2 g, ODR/2 bandwidth.
    // CTRL1: ODR = 100 Hz (0b0101), high-performance mode (0b01).
    if (!writeRegister(kCtrl2, 0x0C) ||
        !writeRegister(kCtrl6, 0x00) ||
        !writeRegister(kCtrl1, 0x54)) {
      return false;
    }

    ready_ = true;
    return true;
  }

  // Returns false for an I2C error or when no new sample is available.
  // Outputs are updated together only after a complete six-byte read.
  bool readAcceleration(float &xG, float &yG, float &zG) {
    if (!ready_) {
      return false;
    }
    uint8_t status = 0;
    if (!readRegisters(kStatus, &status, 1) || (status & 0x01) == 0) {
      return false;
    }

    uint8_t data[6];
    if (!readRegisters(kOutXL, data, sizeof(data))) {
      return false;
    }

    // The signed output is left-aligned: 0.244 mg per 14-bit LSB equals
    // 0.061 mg per unshifted 16-bit LSB, as in lis2dw12_from_fs2_to_mg().
    xG = decodeAxis(data) * 0.000061f;
    yG = decodeAxis(data + 2) * 0.000061f;
    zG = decodeAxis(data + 4) * 0.000061f;
    return true;
  }

 private:
  static constexpr uint8_t kWhoAmI = 0x0F;
  static constexpr uint8_t kCtrl1 = 0x20;
  static constexpr uint8_t kCtrl2 = 0x21;
  static constexpr uint8_t kCtrl6 = 0x25;
  static constexpr uint8_t kStatus = 0x27;
  static constexpr uint8_t kOutXL = 0x28;

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
      if (!Wire.available()) {
        return false;
      }
      data[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
  }

  uint8_t address_ = 0;
  bool ready_ = false;
};
