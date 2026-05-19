# Pico Tracker Hardware Pinout

Raspberry Pi Pico (RP2040) をベースとした Pico Tracker ハードウェアのピンアサイン表です。

## 1. MIDI Module (UART)
| Pico Pin | GPIO | Function | Wire Color | 備考 |
| :--- | :--- | :--- | :--- | :--- |
| 1 | **GP0** | UART0 TX | Orange | MIDI OUT |
| 2 | **GP1** | UART0 RX | Teal | MIDI IN |
| 36 | **3V3(OUT)** | 3.3V Power | Red | |
| 38 | **GND** | Ground | Black | |

## 2. Keypad (9 Keys)
| Pico Pin | GPIO | Key Name (Image) | Wire Color | 備考 |
| :--- | :--- | :--- | :--- | :--- |
| 4 | **GP2** | Left | Blue | Internal Pull-up 推奨 |
| 5 | **GP3** | Down | Blue | Internal Pull-up 推奨 |
| 6 | **GP4** | Right | Blue | Internal Pull-up 推奨 |
| 7 | **GP5** | Up | Blue | Internal Pull-up 推奨 |
| 9 | **GP6** | RT | Blue | Internal Pull-up 推奨 |
| 10 | **GP7** | B | Blue | Internal Pull-up 推奨 |
| 11 | **GP8** | A | Blue | Internal Pull-up 推奨 |
| 12 | **GP9** | LT | Blue | Internal Pull-up 推奨 |
| 14 | **GP10** | Play | Blue | Internal Pull-up 推奨 |
| 3 | **GND** | Common Ground | Black | 全キー共通GND |

~~## 3. Micro SD Card Module (SPI1)~~
*(※ 内蔵フラッシュメモリ(16MB)をストレージとして活用するため、SDカードモジュールは不使用・撤去となりました)*

## 4. ILI9341 3.2" Display (SPI0)
| Pico Pin | GPIO | Function | Wire Color | 備考 |
| :--- | :--- | :--- | :--- | :--- |
| 21 | **GP16** | SPI0 RX / RST | Green | MISO または RESET |
| 22 | **GP17** | SPI0 CSn | Green | CS |
| 24 | **GP18** | SPI0 SCK | Green | SCK |
| 25 | **GP19** | SPI0 TX (MOSI) | Green | MOSI |
| 26 | **GP20** | DC (Data/Cmd) | Green | D/C |
| 36 | **3V3(OUT)** | 3.3V Power | Red | VCC |
| 38 | **GND** | Ground | Black | GND |

## 5. GY-PCM5102 DAC (I2S Audio)
| Pico Pin | GPIO | Function | Wire Color | 備考 |
| :--- | :--- | :--- | :--- | :--- |
| 27 | **GP21** | I2S Data / Clock | Yellow | DIN / BCK / LRCK のいずれか |
| 29 | **GP22** | I2S Data / Clock | Yellow | DIN / BCK / LRCK のいずれか |
| 31 | **GP26** | I2S Data / Clock | Yellow | DIN / BCK / LRCK のいずれか |
| 40 | **VBUS** | 5V Power | Yellow | DACボードのVIN/VCC |
| 23 | **GND** | Ground | Black | GND |

*(※DACのI2Sの3本(GP21, GP22, GP26)は、ソフトウェア側のPIO設定でLRCK/BCK/DINを自由に割り当て可能です。ただしBCLKとLRCKは連番(GP21, GP22)にするのがベストプラクティスです。)*

---

## ⚠️ ベストプラクティス検証と改善の提案

ピンアウトを一般的なベストプラクティス（特にRP2040のハードウェア仕様）と照らし合わせた結果、**1点だけ重大なリスク**があり、他は**非常に優秀な設計**であることがわかりました。

### 🚨 修正すべき点：ディスプレイの3.3V電源負荷
現在の配線では、ILI9341ディスプレイの電源(VCC)がPicoの `3V3(OUT)` (Pin 36) に接続されています。

Picoに内蔵されている3.3Vレギュレータ(RT6150)の最大出力電流は **約300mA** です。
SDカードモジュールを撤去したことで最も危険なスパイク電流（200mA）は無くなりましたが、それでも3.2インチディスプレイのバックライト（約80〜150mA）はPico本体にとって比較的大きな負荷となります。

**👉 改善案**:
一般的なILI9341ディスプレイモジュールには、独自の3.3Vレギュレータ（LDO）が搭載されており、5V入力に対応しています。
安全性を最大限に高めるため、ディスプレイの `VCC`（赤色ワイヤー）は、Pin 36 (`3V3(OUT)`) ではなく、DACと同じ **Pin 40 (`VBUS` : 5V)** に繋ぎ直すことをお勧めします。これによりPico本体のレギュレータへの負荷を完全に逃がすことができます。

### 💡 素晴らしい点（褒められる設計）
1.  **SPI1の完全な解放**: SDカードを撤去したことで `SPI1` が丸ごと空きました！将来的に追加のハードウェア（別のI2C/SPIデバイス等）を繋ぎたくなった場合でも、ディスプレイのSPI0と干渉を気にせず拡張できます。
2.  **I2Sピンの割り当て**: PIOを使用してI2Sを出力する場合、クロックピン（BCLKとLRCK）は「連番のGPIO」であることが求められます。今回 `GP21` と `GP22` が隣接して確保されているため、PIOの実装が非常にスムーズに行えます。(`GP26`をDataピンとして独立させるのは全く問題ありません)
3.  **内蔵プルアップの活用**: キースイッチを直接GNDに落とす設計は、ソフトウェア側で内蔵プルアップ抵抗を有効化すれば良いため、外部抵抗を省ける非常にスマートなベストプラクティスです。
