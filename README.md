# AI05 Env Sensors

温湿度・気圧・3軸加速度を測定する Leafony リーフの KiCad プロジェクトです。

## 構成部品

| 部品 | メーカー | 機能 |
| --- | --- | --- |
| SHT40-AD1B-R2 | Sensirion | 温湿度（選択実装） |
| SGP40-D-R4 | Sensirion | VOC ガス（選択実装） |
| BME280 / BME680 | Bosch Sensortec | 温湿度・気圧 / 温湿度・気圧・ガス（選択実装） |
| LIS2DW12TR | STMicroelectronics | 3軸加速度 |
| SM04B-SRSS-TB | JST | Qwiic、4極、1 mm ピッチ、横出し |

全センサーは Leafony Bus の **3.3 V・I²C** に接続します。基板は **23 × 20 mm、4層、板厚 0.8 mm** です。

SHT40とSGP40は現行の回路図・BOMではDNPです。標準構成ではBME280とLIS2DW12を実装します。

BME280 と BME680 は専用フットプリントをそれぞれ設け、**片方だけを実装**します。標準構成は BME280（U3、C5、C6）を実装し、BME680（U4、C7、C8）を DNP とします。BME680 構成ではこの実装・未実装を入れ替えます。

| センサー | I²C アドレス |
| --- | --- |
| SHT40 | `0x44` |
| SGP40 | `0x59` |
| BME280 / BME680 | `0x76` |
| LIS2DW12 | `0x18`（SDO/SA0をGNDに接続） |

LIS2DW12 の割り込み端子はD3に接続されます。I2C通信速度の上限は 400 kHz です。

SDA/SCL のプルアップはAP03 STM32リーフに搭載されているためDNPとします。既存 AI01 4-Sensors とは加速度センサーの割り込みピンD3が重複するため、同じバスに同時接続しないでください。

## サンプルコード

[`platformio/`](platformio/README.md) にサンプルをフォルダごとの独立したPlatformIOプロジェクトとして配置しています。[`read_sensors`](platformio/read_sensors/README.md) はAP03 STM32向けに、起動時にセンサーを検出・初期化し、1秒ごとに測定値をシリアル（115200 bps）へ出力します。未実装センサーはスキップします。

[`accel_wakeup`](platformio/accel_wakeup/README.md) はLIS2DW12の動き検出をD3の割り込みで受け取り、検出時刻・回数・軸・加速度をシリアル出力します。

## ライセンス

MIT
