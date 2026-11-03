# AI05 Env Sensors サンプル

Leafony AP03 STM32とAI05 Env Sensorsを接続し、起動時にセンサーを検出・初期化して、1秒ごとに測定値をシリアルへ出力します。PlatformIO + Arduinoで動作するプロジェクトです。

## 対象センサー

| センサー | I²Cアドレス | 現行BOMの実装 | 出力 |
| --- | --- | --- | --- |
| BME280 | `0x76` | 実装 | 温度（°C）、湿度（%RH）、気圧（hPa） |
| LIS2DW12 | `0x18` | 実装 | X/Y/Z加速度（g） |
| SHT40 | `0x44` | DNP | 温度（°C）、湿度（%RH） |
| SGP40 | `0x59` | DNP | VOC生信号 `SRAW_VOC`（ticks） |
| BME680 | `0x76` | DNP、BME280と選択実装 | 温度（°C）、湿度（%RH）、気圧（hPa）、ガス抵抗（kΩ） |

BME280とBME680は同じアドレスのため、片方のみを実装してください。サンプルはチップIDから自動判別します。LIS2DW12はWHO_AM_Iも確認し、現行基板のSA0=GND（`0x18`）を優先します。SA0=High（`0x19`）にも対応します。

## 接続

- CPU：AP03 STM32（`leafony_ap03`）。他のCPUリーフは本プロジェクトの対象外です。
- 電源：3.3 V、GND共通。
- SDA：AP03のPB9（D14）、SCL：PB8（D15）。通信速度400 kHz。
- AI05のI²Cプルアップ抵抗はDNPです。AP03側のプルアップを使用します。
- シリアル：AP03のD0/RX（PA3）・D1/TX（PA2）をUSBシリアルリーフ等でPCに接続。115200 bps、8N1。
- LIS2DW12はポーリングで読み出します。D3の割り込みは使用しません。

## ビルド・書き込み・モニター

PlatformIO CoreまたはVS CodeのPlatformIO IDEを用意し、この`platformio/read_sensors`ディレクトリをプロジェクトとして開きます。依存ライブラリは`platformio.ini`の指定から自動インストールされます。

リポジトリのルートから実行する場合：

```sh
# ビルド
pio run -d platformio/read_sensors

# USBシリアルポートを確認
pio device list

# 書き込み（ポート名は実機に合わせる）
pio run -d platformio/read_sensors -t upload --upload-port /dev/cu.usbserial-XXXX

# シリアルモニター（終了: Ctrl+C）
pio device monitor -d platformio/read_sensors --port /dev/cu.usbserial-XXXX --baud 115200
```

Windowsではポートに`COM3`等を指定します。アップロード前にシリアルモニターを閉じてください。`upload_protocol = serial`を指定しているため、AP03をシリアルブートローダーで書き込める状態にする必要があります。使用するUSBリーフで自動書き込みモードへの切り替えができない場合は、BOOT0をHighにしてリセットしてから書き込み、完了後にBOOT0をLowに戻してリセットしてください。モニターを開いた後にリセットすると起動時の検出結果も確認できます。

ST-LINKを使用する場合は、`platformio.ini`の`upload_protocol`を`stlink`に変更します。詳細は[LeafonyのPlatformIO設定手順](https://docs.leafony.com/environment/stm32/platformio/)と[PlatformIOのAP03ボード仕様](https://docs.platformio.org/en/latest/boards/ststm32/leafony_ap03.html)を参照してください。本プロジェクトはPlatformIO同梱のボード定義を使用し、I²Cピンをソースで明示しています。

## 出力例

標準実装での表示例です。数値は説明用です。

```text
AI05 Env Sensors / Leafony AP03
I2C: SDA=PB9, SCL=PB8, 400 kHz; sample interval: 1000 ms
BME280 (0x76): ready
SHT40: not detected at 0x44; skipped (optional)
SGP40: not detected at 0x59; skipped (optional)
LIS2DW12: ready at 0x18

--- uptime=2100 ms ---
BME280: temperature=24.15 C, humidity=45.20 %RH, pressure=1013.25 hPa
LIS2DW12: x=0.010 g, y=-0.005 g, z=0.998 g
```

## 動作と変更箇所

- 測定周期は`src/main.cpp`の`kSampleIntervalMs`で変更できます。周期は各回の読み出し開始時刻を基準にし、処理時間が周期を超えた場合は完了後に次の読み出しを開始します。
- LIS2DW12は高性能モード、出力データレート100 Hz、範囲±2 gで測定し、毎秒最新の値を読み出します。重力加速度を含むため、静止中も各軸がすべて0 gになるわけではありません。
- SHT40は高精度・ヒーターOFFです。BME680のガス測定用ヒーターは320 °C・150 msです。
- SGP40にはその周期の温湿度を補償値として渡します。SHT40、BME280/BME680、既定値25 °C・50 %RHの順に選択し、補償元も表示します。
- SGP40の生信号とBME680のガス抵抗は、VOC Index、IAQスコア、VOC濃度（ppm）ではありません。指数化のアルゴリズムはこのサンプルには含めていません。
- 未実装センサーは起動時にスキップします。初期化済みセンサーの読み出しエラーを検出すると`read failed`を出力し、他のセンサーの読み出しを続けます。BME680のガス測定値が無効な場合は`unavailable`と表示します。センサー構成を変更した場合はリセットして再検出してください。

`src/Lis2dw12.h`はI²CのNACK・短い読み出しを検出する小さなドライバーです。レジスタ設定と加速度換算は[ST公式ドライバー](https://github.com/stm32duino/LIS2DW12)を参照しています。他のセンサーにはAdafruitの各ライブラリを使用します。
