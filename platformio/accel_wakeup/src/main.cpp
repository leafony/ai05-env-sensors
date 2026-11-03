#include <Arduino.h>
#include <Wire.h>

#include "Lis2dw12Wakeup.h"

namespace {
// AI05のLIS2DW12 INT1はLeafony Bus D3、AP03ではPB3に接続される。
constexpr uint32_t kInterruptPin = PB3;
constexpr uint32_t kI2cClockHz = 400000;

// ±2 gでは1カウント = 2/64 g。8カウント = 0.250 g。
constexpr uint8_t kWakeupThresholdCounts = 8;  // 1..63
// 100 Hzでは1カウント = 10 ms。0は追加の継続時間なし。
constexpr uint8_t kWakeupDurationCounts = 2;  // 0..3 (0/10/20/30 ms)
static_assert(kWakeupThresholdCounts >= 1 && kWakeupThresholdCounts <= 63,
              "Wake-up threshold must be 1..63");
static_assert(kWakeupDurationCounts <= 3, "Wake-up duration must be 0..3");

Lis2dw12Wakeup accelerometer;
bool sensorReady = false;
uint32_t wakeupCount = 0;
volatile bool interruptPending = false;
volatile uint32_t interruptTimeMs = 0;

void onWakeupInterrupt() {
  // ISR内ではI2C通信・Serial出力・delayを行わない。
  if (!interruptPending) {
    interruptTimeMs = millis();
    interruptPending = true;
  }
}

bool takePendingInterrupt(uint32_t &timeMs) {
  // 時刻とフラグをまとめて受け取る。割り込み禁止区間は短く保つ。
  noInterrupts();
  const bool pending = interruptPending;
  if (pending) {
    timeMs = interruptTimeMs;
    interruptPending = false;
  }
  interrupts();
  return pending;
}

void retryPendingInterrupt(uint32_t timeMs) {
  // 読み出し失敗でINT1がHighのままだと、次の立ち上がりは来ない。
  // 新しい通知がなければ元の通知を戻し、通常処理で再試行する。
  noInterrupts();
  if (!interruptPending) {
    interruptTimeMs = timeMs;
    interruptPending = true;
  }
  interrupts();
}

void printAxes(uint8_t source) {
  if (source & Lis2dw12Wakeup::kWakeupX) {
    Serial.print('X');
  }
  if (source & Lis2dw12Wakeup::kWakeupY) {
    Serial.print('Y');
  }
  if (source & Lis2dw12Wakeup::kWakeupZ) {
    Serial.print('Z');
  }
  if ((source & (Lis2dw12Wakeup::kWakeupX | Lis2dw12Wakeup::kWakeupY |
                 Lis2dw12Wakeup::kWakeupZ)) == 0) {
    Serial.print('-');
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nAI05 accel_wakeup / Leafony AP03");
  Serial.println("I2C: SDA=PB9, SCL=PB8; INT1=D3/PB3 (rising edge)");
  Serial.println("Keep the board still while initializing.");

  pinMode(kInterruptPin, INPUT);  // センサー側はactive-high、push-pull。
  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();
  Wire.setClock(kI2cClockHz);

  uint8_t detectedAddress = 0;
  const uint8_t addresses[] = {0x18, 0x19};
  for (uint8_t address : addresses) {
    if (accelerometer.begin(address, kWakeupThresholdCounts, kWakeupDurationCounts)) {
      detectedAddress = address;
      break;
    }
  }
  if (detectedAddress == 0) {
    Serial.println("LIS2DW12: initialization failed at 0x18/0x19. Check wiring and reset.");
    return;
  }

  // センサーのINT1出力を有効にする前にMCU側の割り込みを登録する。
  attachInterrupt(digitalPinToInterrupt(kInterruptPin), onWakeupInterrupt, RISING);
  if (!accelerometer.enableWakeupInterrupt()) {
    detachInterrupt(digitalPinToInterrupt(kInterruptPin));
    Serial.println("LIS2DW12: interrupt setup failed. Check I2C and reset.");
    return;
  }
  sensorReady = true;

  Serial.print("LIS2DW12: ready at 0x");
  Serial.println(detectedAddress, HEX);
  Serial.print("Wake-up: threshold=");
  Serial.print(kWakeupThresholdCounts * Lis2dw12Wakeup::kThresholdStepG, 3);
  Serial.print(" g, duration=");
  Serial.print(kWakeupDurationCounts * 1000 / Lis2dw12Wakeup::kOutputDataRateHz);
  Serial.print(" ms, ODR=");
  Serial.print(Lis2dw12Wakeup::kOutputDataRateHz);
  Serial.println(" Hz, range=+/-2 g");
  Serial.println("Move the board to trigger an interrupt. MCU stays awake.");
}

void loop() {
  if (!sensorReady) {
    delay(100);
    return;
  }

  uint32_t eventTimeMs = 0;
  if (!takePendingInterrupt(eventTimeMs)) {
    delay(1);
    return;
  }

  // 読み出しは通知がある時だけ行う。原因レジスタの読み出しでラッチ解除。
  uint8_t source = 0;
  if (!accelerometer.readWakeupSource(source)) {
    Serial.println("LIS2DW12: wake-up source read failed; retrying in 100 ms");
    retryPendingInterrupt(eventTimeMs);
    delay(100);
    return;
  }
  if ((source & Lis2dw12Wakeup::kWakeupEvent) == 0) {
    return;  // Wake-up以外の通知では回数を増やさない。
  }

  ++wakeupCount;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  const bool hasAcceleration = accelerometer.readAcceleration(x, y, z);

  Serial.print("Wake-up #");
  Serial.print(wakeupCount);
  Serial.print(": time=");
  Serial.print(eventTimeMs);
  Serial.print(" ms, axes=");
  printAxes(source);
  // 表示値は処理時点の加速度（重力を含む）。検出した瞬間のピーク値ではない。
  if (hasAcceleration) {
    Serial.print(", x=");
    Serial.print(x, 3);
    Serial.print(" g, y=");
    Serial.print(y, 3);
    Serial.print(" g, z=");
    Serial.print(z, 3);
    Serial.println(" g");
  } else {
    Serial.println(", acceleration=unavailable (I2C read failed)");
  }
}
