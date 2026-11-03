# PlatformIO サンプル

各サンプルを独立したPlatformIOプロジェクトとして、サブフォルダに配置しています。VS Codeでは、実行したいサンプルのフォルダを開いてください。

| サンプル | 内容 | 対象CPU |
| --- | --- | --- |
| [read_sensors](read_sensors/README.md) | センサーを検出・初期化し、1秒ごとに測定値をシリアル出力 | AP03 STM32 |
| [accel_wakeup](accel_wakeup/README.md) | 動き検出の割り込みで時刻・回数・検出軸・加速度をシリアル出力 | AP03 STM32 |

```text
platformio/
├── README.md
├── accel_wakeup/
│   ├── README.md
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp
│       └── Lis2dw12Wakeup.h
└── read_sensors/
    ├── README.md
    ├── platformio.ini
    └── src/
        ├── main.cpp
        └── Lis2dw12.h
```

リポジトリのルートからセンサー読み出しサンプルをビルドする場合：

```sh
pio run -d platformio/read_sensors
```

新しいサンプルを追加するときは、`platformio/<サンプル名>/`に`platformio.ini`、`src/`、使い方を記した`README.md`を配置し、上の一覧に追加してください。各プロジェクトの`.pio/`と`.vscode/`は共通の`.gitignore`で除外します。
