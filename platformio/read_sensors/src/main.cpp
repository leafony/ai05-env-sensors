#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BME680.h>
#include <Adafruit_SGP40.h>
#include <Adafruit_SHT4x.h>
#include <math.h>

#include "Lis2dw12.h"

namespace {
constexpr uint32_t kSampleIntervalMs = 1000;
constexpr uint32_t kI2cClockHz = 400000;
constexpr uint8_t kBmeAddress = 0x76;
constexpr uint8_t kSht40Address = 0x44;
constexpr uint8_t kSgp40Address = 0x59;

Adafruit_BME280 bme280;
Adafruit_BME680 bme680(&Wire);
Adafruit_SHT4x sht40;
Adafruit_SGP40 sgp40;
Lis2dw12 accelerometer;

bool hasBme280 = false;
bool hasBme680 = false;
bool hasSht40 = false;
bool hasSgp40 = false;
bool hasAccelerometer = false;
uint32_t previousSampleMs = 0;

bool isPresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

void printInitResult(const char *name, bool success) {
  Serial.print(name);
  Serial.println(success ? ": ready" : ": initialization failed");
}

void printReadError(const char *name) {
  Serial.print(name);
  Serial.println(": read failed");
}

void printTemperatureHumidity(float temperature, float humidity) {
  Serial.print("temperature=");
  Serial.print(temperature, 2);
  Serial.print(" C, humidity=");
  Serial.print(humidity, 2);
  Serial.print(" %RH");
}

bool validTemperatureHumidity(float temperature, float humidity) {
  return isfinite(temperature) && isfinite(humidity) &&
         temperature >= -40.0f && temperature <= 125.0f &&
         humidity >= 0.0f && humidity <= 100.0f;
}

void initializeSensors() {
  // BME280/BME680 は同一アドレス。チップIDを調べてから対応する初期化を行う。
  uint8_t chipId = 0;
  if (!readRegister(kBmeAddress, 0xD0, chipId)) {
    Serial.println("BME280/BME680: not detected at 0x76; skipped");
  } else if (chipId == 0x60) {
    hasBme280 = bme280.begin(kBmeAddress, &Wire);
    if (hasBme280) {
      bme280.setSampling(Adafruit_BME280::MODE_NORMAL,
                         Adafruit_BME280::SAMPLING_X2,
                         Adafruit_BME280::SAMPLING_X4,
                         Adafruit_BME280::SAMPLING_X2,
                         Adafruit_BME280::FILTER_X4,
                         Adafruit_BME280::STANDBY_MS_500);
    }
    printInitResult("BME280 (0x76)", hasBme280);
  } else if (chipId == 0x61) {
    hasBme680 = bme680.begin(kBmeAddress);
    if (hasBme680) {
      bme680.setTemperatureOversampling(BME680_OS_8X);
      bme680.setHumidityOversampling(BME680_OS_2X);
      bme680.setPressureOversampling(BME680_OS_4X);
      bme680.setIIRFilterSize(BME680_FILTER_SIZE_3);
      bme680.setGasHeater(320, 150);
    }
    printInitResult("BME680 (0x76)", hasBme680);
  } else {
    Serial.print("BME280/BME680: unsupported chip ID 0x");
    Serial.println(chipId, HEX);
  }

  if (isPresent(kSht40Address)) {
    hasSht40 = sht40.begin(&Wire);
    if (hasSht40) {
      sht40.setPrecision(SHT4X_HIGH_PRECISION);
      sht40.setHeater(SHT4X_NO_HEATER);
    }
    printInitResult("SHT40 (0x44)", hasSht40);
  } else {
    Serial.println("SHT40: not detected at 0x44; skipped (optional)");
  }

  if (isPresent(kSgp40Address)) {
    hasSgp40 = sgp40.begin(&Wire);
    printInitResult("SGP40 (0x59)", hasSgp40);
  } else {
    Serial.println("SGP40: not detected at 0x59; skipped (optional)");
  }

  // 現行基板は SA0=GND (0x18)。SA0=High の実装にも対応する。
  const uint8_t accelerometerAddresses[] = {0x18, 0x19};
  for (uint8_t address : accelerometerAddresses) {
    if (isPresent(address) && accelerometer.begin(address)) {
      hasAccelerometer = true;
      Serial.print("LIS2DW12: ready at 0x");
      Serial.println(address, HEX);
      break;
    }
  }
  if (!hasAccelerometer) {
    Serial.println("LIS2DW12: not detected or initialization failed at 0x18/0x19");
  }

  if (!hasBme280 && !hasBme680 && !hasSht40 && !hasSgp40 && !hasAccelerometer) {
    Serial.println("No sensors initialized. Check power, SDA/SCL and pull-ups, then reset.");
  }
}

void sampleSensors() {
  Serial.print("\n--- uptime=");
  Serial.print(millis());
  Serial.println(" ms ---");

  // 補償値は各周期で更新。取得失敗時に前回値を使い回さない。
  float compensationTemperature = 25.0f;
  float compensationHumidity = 50.0f;
  const char *compensationSource = "default 25 C / 50 %RH";

  if (hasBme280) {
    if (isPresent(kBmeAddress)) {
      const float temperature = bme280.readTemperature();
      const float humidity = bme280.readHumidity();
      const float pressure = bme280.readPressure() / 100.0f;
      if (validTemperatureHumidity(temperature, humidity) &&
          isfinite(pressure) && pressure > 0.0f) {
        Serial.print("BME280: ");
        printTemperatureHumidity(temperature, humidity);
        Serial.print(", pressure=");
        Serial.print(pressure, 2);
        Serial.println(" hPa");
        compensationTemperature = temperature;
        compensationHumidity = humidity;
        compensationSource = "BME280";
      } else {
        printReadError("BME280");
      }
    } else {
      printReadError("BME280");
    }
  }

  if (hasBme680) {
    if (isPresent(kBmeAddress) && bme680.performReading() &&
        validTemperatureHumidity(bme680.temperature, bme680.humidity) &&
        isfinite(bme680.pressure) && bme680.pressure > 0.0f) {
      Serial.print("BME680: ");
      printTemperatureHumidity(bme680.temperature, bme680.humidity);
      Serial.print(", pressure=");
      Serial.print(bme680.pressure / 100.0f, 2);
      Serial.print(" hPa, gas_resistance=");
      // ライブラリは有効なガス測定値がない場合に0を返す。
      if (bme680.gas_resistance != 0) {
        Serial.print(bme680.gas_resistance / 1000.0f, 2);
        Serial.println(" kOhm");
      } else {
        Serial.println("unavailable (heater warming up or invalid reading)");
      }
      compensationTemperature = bme680.temperature;
      compensationHumidity = bme680.humidity;
      compensationSource = "BME680";
    } else {
      printReadError("BME680");
    }
  }

  if (hasSht40) {
    sensors_event_t humidity = {};
    sensors_event_t temperature = {};
    if (sht40.getEvent(&humidity, &temperature) &&
        validTemperatureHumidity(temperature.temperature, humidity.relative_humidity)) {
      Serial.print("SHT40: ");
      printTemperatureHumidity(temperature.temperature, humidity.relative_humidity);
      Serial.println();
      compensationTemperature = temperature.temperature;
      compensationHumidity = humidity.relative_humidity;
      compensationSource = "SHT40";
    } else {
      printReadError("SHT40");
    }
  }

  if (hasSgp40) {
    const uint16_t raw = sgp40.measureRaw(compensationTemperature, compensationHumidity);
    if (raw != 0) {
      Serial.print("SGP40: SRAW_VOC=");
      Serial.print(raw);
      Serial.print(" ticks, compensation=");
      Serial.println(compensationSource);
    } else {
      printReadError("SGP40");
    }
  }

  if (hasAccelerometer) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (accelerometer.readAcceleration(x, y, z)) {
      Serial.print("LIS2DW12: x=");
      Serial.print(x, 3);
      Serial.print(" g, y=");
      Serial.print(y, 3);
      Serial.print(" g, z=");
      Serial.print(z, 3);
      Serial.println(" g");
    } else {
      printReadError("LIS2DW12");
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);  // 電源投入直後のセンサー安定とシリアル接続を待つ。
  Serial.println("\nAI05 Env Sensors / Leafony AP03");
  Serial.print("I2C: SDA=PB9, SCL=PB8, ");
  Serial.print(kI2cClockHz / 1000);
  Serial.print(" kHz; sample interval: ");
  Serial.print(kSampleIntervalMs);
  Serial.println(" ms");

  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();
  Wire.setClock(kI2cClockHz);
  initializeSensors();
  Wire.setClock(kI2cClockHz);
  // 最初の変換完了を待ち、以降は読み出し開始時刻を基準に周期を維持する。
  previousSampleMs = millis();
}

void loop() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - previousSampleMs) >= kSampleIntervalMs) {
    previousSampleMs = now;  // millis() の桁あふれにも対応。
    sampleSensors();
  }
  delay(1);
}
