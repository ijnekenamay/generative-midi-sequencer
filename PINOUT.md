# Generative MIDI Sequencer — Hardware Pinout

Raspberry Pi Pico (RP2040) / Pimoroni Pico LiPo をベースとした Pico Tracker ハードウェアのピンアサイン表です。
操作方法は [STARTUP_GUIDE.md](STARTUP_GUIDE.md) を参照してください。

---

## 物理ピン配置図

```text
       [ Pimoroni Pico LiPo / Raspberry Pi Pico ]
         +-----------------+
    TX0  | [GP0]     [VBUS] | ───> DAC 5V 電源 (VIN)
    RX0  | [GP1]     [VSYS] |
   Left  | [GP2]    [3V3_O] | ───> ディスプレイ / MIDI / 共通3.3V
   Down  | [GP3]     [GND]  | ───> 共通 GND
  Right  | [GP4]    [GP28]  |
     Up  | [GP5]    [GP27]  |
     RT  | [GP6]    [GP26]  | ───> DAC I2S DATA (DIN)
      B  | [GP7]     [RUN]  |
      A  | [GP8]    [GP22]  | ───> DAC I2S LRCK
     LT  | [GP9]    [GP21]  | ───> DAC I2S BCLK
   Play  | [GP10]   [GND]   |
  TFT_CS | [GP11]   [GP20]  | ───> TFT D/C (Data/Command)
 TFT_SCK | [GP12]   [GP19]  | ───> TFT_MOSI (SPI0 TX)
TFT_MISO | [GP13]   [GP18]  | ───> TFT_SCK (SPI0 CLK)
 TFT_DC  | [GP14]   [GP17]  | ───> TFT_RST (Reset)
         | [GP15]   [GP16]  |
         +-----------------+
```

> **注意**: ファームウェアの実装は `main.cpp` の `init_pio_i2s()` と `DisplayController` 初期化ルーチンを参照してください。

---

## 1. MIDI / UART モジュール

| Pico Pin | GPIO | 機能 | 備考 |
|:--|:--|:--|:--|
| 1 | **GP0** | UART0 TX — MIDI OUT | 220Ω 抵抗を介してTRS-MIDIジャック Tip へ接続 |
| 2 | **GP1** | UART0 RX — (未使用/将来の MIDI IN 拡張用) | 現バージョンでは受信未実装 |

---

## 2. キースイッチ（9キー）

全スイッチは内蔵プルアップ抵抗（ソフトウェア設定）を使用。スイッチ片側を GPIO へ、もう片側を **GND** に接続（アクティブ LOW）。

| Pico Pin | GPIO | キー名 | 単体動作 | LT + 同時押し |
|:--|:--|:--|:--|:--|
| 4 | **GP2** | LEFT | パラメータを左へ移動 | — |
| 5 | **GP3** | DOWN | トラックを下へ移動 | BPM -1 |
| 6 | **GP4** | RIGHT | パラメータを右へ移動 | — |
| 7 | **GP5** | UP | トラックを上へ移動 | BPM +1 |
| 9 | **GP6** | RT | ミュート / ミュート解除 | — |
| 10 | **GP7** | B | パラメータ値を減少 | 高速減少 |
| 11 | **GP8** | A | パラメータ値を増加 | 高速増加 |
| 12 | **GP9** | LT | Shift/Modifier キー（ホールド） | — |
| 14 | **GP10** | PLAY | 再生 / 停止 | フラッシュ保存 |
| 3 | **GND** | — | 全キー共通 GND | — |

---

## 3. ILI9341 3.2" TFT LCD（SPI0）

SPI0 バスを使用。48 MHz 転送で高速 UI 描画を実現します。

| Pico Pin | GPIO | LCD ピン名 | 備考 |
|:--|:--|:--|:--|
| 22 | **GP17** | CS (Chip Select) | — |
| 24 | **GP18** | SCK | — |
| 25 | **GP19** | MOSI (SDI) | — |
| 26 | **GP20** | D/C (Data/Command) | — |
| 22 | **GP17** | RST (Reset) | *(注: RST は GP17 とは別ピンが望ましい。現状は兼用)* |
| 36 | **3V3(OUT)** | VCC | または VBUS(5V) 経由 LDO 推奨（後述） |
| 38 | **GND** | GND | — |

---

## 4. GY-PCM5102 I2S DAC（アナログクロック出力）

PIO0 のカスタム I2S ステートマシンで駆動。22.05 kHz, 16-bit ステレオ出力として MIDI クロックに同期したアナログパルスを送出します。

| Pico Pin | GPIO | I2S 信号 | 備考 |
|:--|:--|:--|:--|
| 27 | **GP21** | BCLK (Bit Clock) | PIO side-set ピン |
| 29 | **GP22** | LRCK (Left-Right Clock) | PIO side-set ピン |
| 31 | **GP26** | DIN (Data) | PIO output ピン |
| 40 | **VBUS** | VIN (5V 電源) | DAC ボードの 5V 入力 |
| 23 | **GND** | GND | — |

> BCLK と LRCK は連番の GPIO（GP21, GP22）を使用しているため、PIO 実装が最適化されています。

**アナログ出力の使い方**: DAC の L/R いずれかのアナログ出力（通常 3.3V フルスケール）をユーロラックや外部機器の Clock/Run 入力に接続してください。

---

## ⚠️ ハードウェア設計上の注意点

### 🚨 ディスプレイの電源供給

現状の配線では ILI9341 の VCC が Pico の `3V3(OUT)` (Pin 36) に接続されています。Pico の内蔵 3.3V レギュレータ (RT6150) の最大出力は約 300mA ですが、3.2インチ LCD のバックライトは 80〜150mA を消費するため、比較的大きな負荷となります。

**👉 推奨改善**: 多くの ILI9341 モジュールは独自の LDO を搭載しており 5V 入力に対応しています。LCD の VCC を `3V3(OUT)` ではなく `VBUS` (Pin 40, 5V) に繋ぎ直すことで、Pico のレギュレータへの負荷を完全に解消できます。

### 💡 設計上の良好な点

1. **SPI1 の完全解放**: 内蔵フラッシュをストレージに使用しているため SPI1 は空き。将来の拡張が容易。
2. **I2S ピンの連番確保**: BCLK (GP21) と LRCK (GP22) が隣接しており、PIO の side-set 実装が最適。
3. **内蔵プルアップの活用**: キースイッチを直接 GND に落とす設計でソフトウェア側の内蔵プルアップを使用。外部抵抗不要のスマートな設計。
