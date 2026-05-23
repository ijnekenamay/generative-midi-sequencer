# Generative Techno MIDI Sequencer

## プロジェクト概要 (Overview)

本プロジェクトは、Raspberry Pi Pico (RP2040) のデュアルコアアーキテクチャを活かした、ハードウェア動作の **4チャンネル・ジェネレーティブMIDIシーケンサー** です。
ユーロラックの定番モジュール「Turing Machine」にインスパイアされた確率的シフトレジスタ（S&Hエミュレーション）と、Bjorklundアルゴリズムによるユークリッドリズム生成を組み合わせ、偶発的かつ極めて音楽的なテクノ・ミニマル系のシーケンスを自律的に生成します。

本システムは、余った **[「Pico Tracker」ハードウェア（3.2インチ ILI9341 液晶画面、9つの Kailh Choc V1 キースイッチ、PCM5102 I2S DAC搭載）](https://github.com/ijnekenamay/picotracker_alt-pcb)** を流用する形でファームウェアが最適化されており、人間工学に基づいた高コントラストのモノクロームUIからリアルタイムに操作可能です。

![UI](uiai.png)

---

## 🛠 実装済みのコア機能仕様 (Current Implementation)

### 1. 4ch 独立 MIDIシーケンサー & クロック同期

- **4つの完全独立トラック**: 全トラックが独立したシーケンスエンジン（`Track 1`〜`Track 4`）として動作。
- **トラック独立のクロック・ディバイド（倍率設定）**: マスタークロックに対して各トラックごとに個別の倍率（`x1`, `x2`, `x3`, `x4`, `x6`, `x8`, `/2` など）を設定可能。ポリリズム構築に強力。
- **ステップ長の個別設定 (LEN)**: 各チャンネルごとに `1`〜`32` ステップでリアルタイム変更可能。
- **標準 MIDI クロック送信**: UART0 (GP0 TX) を経由して 31,250 bps の標準シリアル MIDI 信号を出力（MIDI Start / Stop / Clock 同期対応）。

### 2. ジェネレーティヴ・リズム & ピッチ生成エンジン

- **ユークリッド・リズムジェネレータ (`EuclideanGenerator`)**: Bresenhamアルゴリズムを応用した軽量リアルタイム計算。ステップ数 (Length)、パルス数 (Density)、開始シフト (Shift) の3パラメータで制御。
- **確率的シフトレジスタピッチジェネレータ (`NoteGenerator`)**: 32-bitシフトレジスタを内蔵。**Mutation（変異確率）** パラメータにより 0〜100% の確率でビット変異を制御。
- **高精度スケール・クォンタイザー**: 生成されたランダムなCV値を選択したスケールおよびルート音に量子化。以下の **9種**のスケールを瞬時に切り替え可能：

  | # | UI | スケール名 | 音楽的キャラクター |
  |:-:|:-:|:--|:--|
  | S1 | `CHROM` | Chromatic | 半音階 |
  | S2 | `MINOR` | Natural Minor | 定番マイナー |
  | S3 | `PHRYG` | Phrygian | ダーク・アシッド |
  | S4 | `DORIN` | Dorian | ディープ・テクノ |
  | S5 | `PENTA` | Minor Pentatonic | ブルーノート |
  | S6 | `HUNG` | Hungarian Minor | エキゾチック・ダーク |
  | S7 | `WHOLE` | Whole Tone | 浮遊・夢幻 |
  | S8 | `BLUES` | Blues | ブルースノート(♭5) |
  | S9 | `MAJP` | Major Pentatonic | 明るい開放感 |

### 3. 音楽的グルーヴ生成

- **自動ベロシティ・ダイナミクス**: Turing Machine のシフトレジスタ値をベロシティに連動させ、メトリック位置（ダウンビート・クォーターノート）で自動アクセント付与。MUT=0（ロック）時は固定グルーヴ、MUTが上がるほど有機的に変化。設定項目は不要。
- **確率的・非同期ゲート長制御 (GAT)**: 各ステップのゲート長を 10〜100% で設定。SLモード（GAT=0）でスタッカート/レガートをランダム切替。
- **リアルタイム可変マスターBPM**: `40`〜`250` の範囲でリアルタイムにテンポ調節可能。

### 4. Elektron スタイル発音確率制御 (PRB)

ユークリッドリズムのヒット時に確率的な「間引き」を加える **PRB（発音確率）** パラメータ。
- `FL`（100%）= 常に発音
- 値を下げると Euclidean ヒットが確率的にスキップされる
- Turing Machine のシフトレジスタは常に進行するため、音列の継続性を保ちながら「抜け感」が生まれる

### 5. ハードウェアの潜在能力を引き出すマルチコア・IPC設計

- **スピンロック付き共有メモリ (Spinlock IPC)**: Core 0 (UI) と Core 1 (リアルタイムエンジン) 間のデータ競合を完全に防止。
- **アナログ・クロック同期出力 (I2S DAC経由)**: マスタークロックと100%同期したアナログクロックパルスを I2S DAC (GP17 Data) から出力。
- **安全なマルチコア・ロックアウトによるフラッシュセーブ**: 内蔵フラッシュの最終セクター (`0xFFF000`) への保存時は Pico SDK Multicore Lockout API でCore 1を完全一時停止させ、XIPクラッシュを防止。

### 6. 人間工学に基づく極細モノクローム・UI

- **ヘッダーエリア**: BPM表示（拍に同期してネガポジ反転フラッシュ）、スケール名、再生状態、セーブ状態を表示。
- **8列パラメータグリッド (SPD, LEN, DEN, SHF, MUT, PRB, GAT, ROT, SCL / RND)**: 選択セルはシアン枠線でハイライト。各コラムにカスタム8x8ピクセルアートアイコンを表示。
- **ステップシーケンスレーン**: 各トラックの現在ステップと Euclidean パターンをリアルタイム可視化。差分描画でSPI負荷を最小化。

---

## 🎛 物理キーマッピング (9-Key Controls)

| 物理キー名 | Pico GPIO | 単体押しの動作 | `LT` (Shift) 同時押しの動作 |
|:--|:--|:--|:--|
| **UP** | GP5 | カーソルを上のトラックに移動 | グローバルBPMを **+1** 増やす |
| **DOWN** | GP3 | カーソルを下のトラックに移動 | グローバルBPMを **-1** 減らす |
| **LEFT** | GP2 | カーソルを左のパラメータに移動 | - |
| **RIGHT** | GP4 | カーソルを右のパラメータに移動 | - |
| **A** | GP8 | 選択パラメータ値を増やす | 高速増加（パラメータ依存） |
| **B** | GP7 | 選択パラメータ値を減らす | 高速減少（パラメータ依存） |
| **RT** | GP6 | **MUTE / UNMUTE** | - |
| **PLAY** | GP10 | シーケンスの一時停止／再生再開 | **フラッシュへ現在設定を保存 (SAVE)** |
| **LT** | GP9 | modifier（Shiftキー）としてホールド | - |

---

## 📂 ディレクトリ構成 (Workspace Layout)

```text
generative-midi-sequencer/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── README.md                   # 本説明書（概要）
├── ARCHITECTURE.md             # ソフトウェア・アーキテクチャ詳細
├── PINOUT.md                   # ハードウェア配線・検証資料
├── STARTUP_GUIDE.md            # ユーザー操作ガイド
└── src/
    ├── CMakeLists.txt
    ├── main.cpp                # エントリーポイント（Core 0/1 の初期化と統合）
    ├── engine/
    │   ├── track.hpp / .cpp          # トラック管理（シーケンス進行・MIDI制御）
    │   ├── euclidean.hpp / .cpp      # ユークリッドリズム計算
    │   ├── note_generator.hpp / .cpp # Turing Machine & スケール量子化
    │   ├── midi_handler.hpp / .cpp   # UART0 31.25kbps シリアルMIDI送信
    │   └── storage_manager.hpp / .cpp # 内蔵フラッシュ保存／ロード
    └── ui/
        ├── display_controller.hpp / .cpp # SPI ILI9341 液晶ドライバ
        ├── input_manager.hpp / .cpp       # 9キー GPIOデバウンス・スキャナー
        └── custom_assets.hpp              # BPMフォント、ピクセルアートアイコン
```

---

## 🚀 ビルドと動作確認方法 (Build Instructions)

### 前提条件

1. **Raspberry Pi Pico C/C++ SDK** がインストールされ、環境変数 `PICO_SDK_PATH` が設定されていること。
2. `arm-none-eabi-gcc` ツールチェーンおよび `CMake` がインストールされていること。

### ビルド実行手順

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### 書き込み手順

ビルド成功後、`build/src/` に `generative_midi_sequencer.uf2` が生成されます。

Pico を **BOOTSELモード**（BOOTSELボタンを押しながらUSB接続）で接続し、`RPI-RP2` ドライブに `generative_midi_sequencer.uf2` をドラッグ＆ドロップしてください。自動的に書き込まれシーケンサーが起動します。

> **詳細ドキュメント**: ハードウェア配線は [PINOUT.md](PINOUT.md)、操作方法は [STARTUP_GUIDE.md](STARTUP_GUIDE.md)、ソフトウェア設計は [ARCHITECTURE.md](ARCHITECTURE.md) を参照してください。
