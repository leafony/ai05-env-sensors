# 加速度センサーの動き検出割り込み（accel_wakeup）

AI05 Env SensorsのLIS2DW12で動きを検出し、INT1割り込みをAP03 STM32で受け取るサンプルです。基板を持ち上げたり揺らしたりすると、検出時刻・回数・検出軸・加速度を115200 bpsのシリアルへ出力します。

このサンプルではSTM32は通常動作を続けます。センサーのWake-up機能を使用し、MCUのSleep／Stopからの復帰は行いません。

## 接続と初期設定

| 項目 | 設定 |
| --- | --- |
| CPU | AP03 STM32（`leafony_ap03`） |
| センサー | LIS2DW12、I²C `0x18`（SA0=Highの`0x19`も検出） |
| I²C | SDA=PB9（D14）、SCL=PB8（D15）、400 kHz |
| 割り込み | LIS2DW12 INT1 → D3（PB3）、立ち上がりエッジ |
| INT1の出力 | Active High、push-pull、原因レジスタを読むまで保持（ラッチ） |
| 測定 | 100 Hz、高性能モード、低ノイズ有効、±2 g |
| 動き検出 | ハイパスフィルターで重力などの定常成分を除去、カットオフ5 Hz |
| しきい値 | 0.250 g |
| 継続時間 | 20 ms |

AI05とAP03をLeafony Busで接続すれば、INT1の追加配線は不要です。3.3 V電源とAP03側のI²Cプルアップを使用します。AI01など、D3を割り込み出力として使うリーフは同時接続しないでください。

## ビルド・書き込み・確認

VS Codeでは`platformio/accel_wakeup/`をプロジェクトとして開きます。Arduinoの`Wire`を直接使うため、追加のセンサーライブラリは不要です。

リポジトリのルートから実行する場合：

```sh
# ビルド
pio run -d platformio/accel_wakeup

# USBシリアルポートを確認
pio device list

# 書き込み（ポート名は実機に合わせる。WindowsではCOM3等）
pio run -d platformio/accel_wakeup -t upload --upload-port /dev/cu.usbserial-XXXX

# シリアルモニター（終了: Ctrl+C）
pio device monitor -d platformio/accel_wakeup --port /dev/cu.usbserial-XXXX --baud 115200
```

書き込み前にシリアルモニターを閉じ、AP03をProgram側にしてResetを押します。完了後はRun側へ戻し、モニターを開いてResetを押してください。接続の詳細は[センサー読み出しサンプル](../read_sensors/README.md)も参照してください。

1. 初期化中は基板を静止させます。フィルター安定のため200 ms待ってから割り込みを有効にします。
2. `LIS2DW12: ready`と`Move the board...`が表示されたら、基板を軽く動かします。
3. `Wake-up #1`のような行が表示され、動きを繰り返すと回数が増えることを確認します。静止中には測定値を定期出力しません。

表示例（数値は説明用）：

```text
AI05 accel_wakeup / Leafony AP03
I2C: SDA=PB9, SCL=PB8; INT1=D3/PB3 (rising edge)
Keep the board still while initializing.
LIS2DW12: ready at 0x18
Wake-up: threshold=0.250 g, duration=20 ms, ODR=100 Hz, range=+/-2 g
Move the board to trigger an interrupt. MCU stays awake.
Wake-up #1: time=3450 ms, axes=X, x=0.320 g, y=0.010 g, z=0.950 g
Wake-up #2: time=7560 ms, axes=XY, x=-0.290 g, y=0.280 g, z=0.960 g
```

`time`は起動から割り込みを受けた時点までのミリ秒、`axes`は検出に関係した軸です。表示する加速度は処理時点のローパス出力で、重力を含みます。割り込みを発生させた瞬間のピーク値やハイパス出力を保存したものではないため、表示値としきい値を直接比較することはできません。

## 感度の変更

`src/main.cpp`の次の定数を変更してビルド・書き込みします。

| 定数 | 初期値 | 範囲と換算 |
| --- | --- | --- |
| `kWakeupThresholdCounts` | `8` | `1..63`。1カウント = 2/64 g = 0.03125 g |
| `kWakeupDurationCounts` | `2` | `0..3`。100 Hzでは0/10/20/30 ms。0は追加の継続時間なし |

反応が弱い場合は、まずしきい値を`4`（0.125 g）へ下げます。短い振動を拾いすぎる場合は、しきい値や継続時間を増やします。ゆっくりした傾きや一定の姿勢はハイパスフィルターで除去されやすく、姿勢検出とは動作が異なります。

## 割り込み処理

`src/Lis2dw12Wakeup.h`がセンサーの初期化とレジスタ操作、`src/main.cpp`がAP03の割り込み受付と出力を担当します。

- MCU側で`attachInterrupt(..., RISING)`を登録してから、センサーのINT1出力を有効にします。
- 割り込みハンドラーでは通知フラグと時刻だけを記録し、I²C通信やシリアル出力は`loop()`で行います。
- 通知を受けた時だけ`WAKE_UP_SRC`を読み、検出軸の取得とラッチ解除を行います。待機中にこのレジスタを繰り返し読みません。
- 原因レジスタの読み出しに失敗した場合は100 ms後に再試行します。ラッチが解除できずINT1がHighのままでも、次の立ち上がりを待って停止しません。
- ラッチと通知フラグはイベントを1件保持する仕組みです。処理中の複数の動きがまとまる場合があり、表示回数は処理できたWake-upイベント数です。
- 初期化エラー時は配線・電源を確認してリセットしてください。加速度の読み出しだけ失敗した場合は`acceleration=unavailable`と表示し、次のイベント受付を続けます。

設定は[ST AN5038のWake-up割り込みとフィルターの説明](https://www.st.com/resource/en/application_note/an5038-lis2dw12-alwayson-3axis-accelerometer-stmicroelectronics.pdf)、[LIS2DW12データシート](https://www.st.com/resource/en/datasheet/lis2dw12.pdf)に基づきます。
