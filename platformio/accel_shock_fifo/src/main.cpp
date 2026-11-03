#include <Arduino.h>
#include <Wire.h>

#include "Lis2dw12ShockFifo.h"
#include "ShockCapture.h"

namespace {
constexpr uint32_t kInterruptPin = PB3;  // LIS2DW12 INT1 -> Leafony D3 -> AP03 PB3
constexpr uint8_t kShockThresholdCounts = 4;  // +/-8 g: 4 * 8/64 g = 0.500 g
constexpr uint32_t kCooldownMs = 500;
static_assert(kShockThresholdCounts >= 1 && kShockThresholdCounts <= 63,
              "Wake-up threshold must be 1..63");

Lis2dw12ShockFifo accelerometer;
ShockCapture capture;
bool sensorReady = false;
bool armed = false;
bool capturing = false;
uint32_t captureId = 0;
uint32_t cooldownStarted = 0;
volatile bool interruptPending = false;
volatile uint32_t interruptTimeMs = 0;

void onShockInterrupt() {
  // No I2C, delay or Serial in the ISR. Keep the sensor's wake-up latch set.
  if (!interruptPending) {
    interruptTimeMs = millis();
    interruptPending = true;
  }
}

bool takeInterrupt(uint32_t &timeMs) {
  noInterrupts();
  const bool pending = interruptPending;
  if (pending) {
    timeMs = interruptTimeMs;
    interruptPending = false;
  }
  interrupts();
  return pending;
}

void clearPendingInterrupt() {
  noInterrupts();
  interruptPending = false;
  interrupts();
}

void fatalError(const char *reason) {
  sensorReady = false;
  armed = false;
  capturing = false;
  detachInterrupt(digitalPinToInterrupt(kInterruptPin));
  uint8_t ignored = 0;
  accelerometer.stopAndClear(ignored);
  Serial.print("# fatal,reason=");
  Serial.println(reason);
  Serial.println("# Check I2C/power and reset the board.");
}

bool armForNextShock() {
  clearPendingInterrupt();
  if (!accelerometer.arm()) {
    fatalError("arm_failed");
    return false;
  }
  armed = true;
  Serial.print("# armed,odr_hz=");
  Serial.print(Lis2dw12ShockFifo::kOutputDataRateHz);
  Serial.print(",range_g=");
  Serial.print(Lis2dw12ShockFifo::kRangeG);
  Serial.print(",threshold_g=");
  Serial.print(kShockThresholdCounts * Lis2dw12ShockFifo::kThresholdStepG, 3);
  Serial.print(",count=");
  Serial.println(ShockCapture::kSampleCount);
  // Flush before returning so no Serial operations occur during acquisition.
  Serial.flush();
  return true;
}

void printCaptureError(const char *reason) {
  Serial.print("# capture_error,id=");
  Serial.print(captureId);
  Serial.print(",reason=");
  Serial.print(reason);
  Serial.print(",collected=");
  Serial.println(capture.count());
}

void printCapture() {
  Serial.print("# capture_begin,id=");
  Serial.print(captureId);
  Serial.print(",trigger_ms=");
  Serial.print(capture.triggerMs());
  Serial.print(",odr_hz=");
  Serial.print(Lis2dw12ShockFifo::kOutputDataRateHz);
  Serial.print(",range_g=");
  Serial.print(Lis2dw12ShockFifo::kRangeG);
  Serial.print(",count=");
  Serial.print(ShockCapture::kSampleCount);
  Serial.println(",source=hardware");
  Serial.println("index,time_ms,x_g,y_g,z_g");
  for (uint16_t i = 0; i < capture.count(); ++i) {
    const FifoSample &sample = capture.samples()[i];
    Serial.print(i);
    Serial.print(',');
    // FIFO has no timestamps. This is relative to the first retained sample,
    // estimated from nominal ODR, NOT time since the IRQ or wall-clock time.
    Serial.print(i * 1000.0f / Lis2dw12ShockFifo::kOutputDataRateHz, 3);
    Serial.print(',');
    Serial.print(sample.x * Lis2dw12ShockFifo::kRawToG, 4);
    Serial.print(',');
    Serial.print(sample.y * Lis2dw12ShockFifo::kRawToG, 4);
    Serial.print(',');
    Serial.println(sample.z * Lis2dw12ShockFifo::kRawToG, 4);
  }
  Serial.print("# capture_end,id=");
  Serial.println(captureId);
}

void finishCapture() {
  // Only now release WAKE_UP_SRC. Clearing it during acquisition would stop
  // bypass-to-continuous FIFO collection and lose the post-trigger waveform.
  uint8_t source = 0;
  if (!accelerometer.stopAndClear(source)) {
    printCaptureError("cleanup_failed");
    fatalError("cleanup_failed");
    return;
  }
  if (capture.state() == ShockCapture::State::Failed) {
    printCaptureError(capture.error());
  } else if ((source & Lis2dw12ShockFifo::kWakeupEvent) == 0) {
    printCaptureError("missing_wakeup_source");
  } else {
    printCapture();
  }
  Serial.flush();
  clearPendingInterrupt();
  capturing = false;
  cooldownStarted = millis();
  // FIFO and INT1 are off throughout output and this cooldown; new impacts
  // during that dead time are intentionally not recorded.
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n# AI05 accel_shock_fifo / Leafony AP03");
  Serial.println("# INT1=D3/PB3, SDA=PB9, SCL=PB8, I2C=400kHz");
  Serial.println("# Keep still while initializing. Capture: 100 post-trigger samples.");
  Serial.println("# time_ms starts at the first retained sample; one initial sample is discarded.");

  pinMode(kInterruptPin, INPUT);
  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();
  Wire.setClock(400000);

  uint8_t detectedAddress = 0;
  const uint8_t addresses[] = {0x18, 0x19};
  for (uint8_t address : addresses) {
    if (accelerometer.begin(address, kShockThresholdCounts)) {
      detectedAddress = address;
      break;
    }
  }
  if (detectedAddress == 0) {
    Serial.println("# fatal,reason=initialization_failed");
    Serial.println("# LIS2DW12 not ready at 0x18/0x19. Check I2C/power and reset.");
    return;
  }
  Serial.print("# LIS2DW12 ready at 0x");
  Serial.println(detectedAddress, HEX);
  sensorReady = true;
  attachInterrupt(digitalPinToInterrupt(kInterruptPin), onShockInterrupt, RISING);
  armForNextShock();
}

void loop() {
  if (!sensorReady) {
    delay(100);
    return;
  }
  if (capturing) {
    capture.poll(accelerometer, millis(), []() { return micros(); });
    if (capture.state() != ShockCapture::State::Collecting) {
      finishCapture();
    } else {
      delay(1);  // Much shorter than the 32 / 400 Hz = 80 ms FIFO capacity.
    }
    return;
  }
  if (!armed) {
    if (static_cast<uint32_t>(millis() - cooldownStarted) >= kCooldownMs) {
      armForNextShock();
    } else {
      delay(1);
    }
    return;
  }
  uint32_t triggerMs = 0;
  if (!takeInterrupt(triggerMs)) {
    delay(1);
    return;
  }
  armed = false;
  capturing = true;
  ++captureId;
  capture.start(triggerMs);
  // Do not read WAKE_UP_SRC here. Its latched event keeps FIFO running.
}
