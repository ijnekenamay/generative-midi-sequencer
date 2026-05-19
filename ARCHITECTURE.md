# Generative MIDI Sequencer - Software Architecture

本ドキュメントでは、Raspberry Pi Pico (RP2040) のデュアルコアを活かしたジェネレーティヴMIDIシーケンサーのソフトウェア設計について定義します。

## 1. デュアルコア・アーキテクチャの役割分担

ジェネレーティヴ計算とUI描画を並行して行うため、RP2040の2つのコアを完全に独立した役割で稼働させます。

### Core 0 (UI & System Thread)
非リアルタイム処理、および負荷の高いI/O処理を担当します。
*   **Display Controller (SPI)**: 3.2インチ ILI9341 への画面描画。トラッカースタイルのUIマトリックスをレンダリングします。
*   **Input Manager**: 9つのキースイッチのスキャン、デバウンス、長押し/同時押し（SHIFT）判定。
*   **Storage Manager**: Pico内蔵フラッシュメモリ（LittleFS）へのプロジェクト/パターンの読み書き。
*   **UI State Manager**: 現在のページ（リズム画面、ピッチ画面）、選択中のトラック、編集中のパラメータ状態を管理します。

### Core 1 (Audio & Realtime MIDI Thread)
タイミングに極めてシビアな処理を担当します。Core 0の処理負荷（画面描画の遅延など）の影響を一切受けないように設計します。
*   **Clock Generator**: ハードウェアタイマー割り込みによる内部クロック生成、または外部MIDI INからのクロック同期処理。
*   **Generative Engine**: 
    *   **Euclidean Generator**: ヒットの有無、確率的ゲート長の計算。
    *   **Note Generator (Turing Machine)**: シフトレジスタの更新とスケール・クォンタイズ。
*   **MIDI UART Handler**: ジッターのないMIDI Note On/Off, CC, クロックのUART送信。
*   **Audio I2S Handler**: PCM5102 DACへのオーディオデータ（プレビュー用シンセサイザーの波形）のDMA転送。

---

## 2. コア間通信 (Inter-Core Communication)

Core 0（UI）で変更されたパラメータを、Core 1（リアルタイムエンジン）へ安全に伝達するための仕組みです。

*   **Shared Memory with Spinlocks (スピンロック付き共有メモリ)**
    *   各トラックのパラメータ（Density, Shift, Mutation, Scale等）はグローバルな構造体として共有メモリに配置します。
    *   読み書きの競合を防ぐため、RP2040のハードウェア・スピンロック（ミューテックス）を使用し、Core 0が値を更新する際と、Core 1が値を読み取る際のデータ整合性を保証します。
*   **Hardware FIFO Queues (ハードウェアFIFO)**
    *   **Core 0 -> Core 1**: 「再生(Play)」「停止(Stop)」「リセット(Reset)」などのワンショットのイベント通知に使用。
    *   **Core 1 -> Core 0**: 「トラック1でノートが発音された」といったUIフィードバック（画面のインジケーターを光らせる等）の通知に使用。

---

## 3. 主要な C++ クラス設計案

```cpp
// --- Global Data Structures ---
struct TrackParams {
    uint8_t length;
    uint8_t density;
    uint8_t shift;
    uint8_t mutation;
    uint8_t rootNote;
    uint8_t scaleId;
    // etc...
};

// --- Core 1 (Realtime) Classes ---
class EuclideanGenerator {
public:
    bool calculateStep(uint8_t step, uint8_t length, uint8_t density, uint8_t shift);
};

class NoteGenerator {
private:
    uint16_t shiftRegister;
public:
    uint8_t getQuantizedNote(uint8_t mutationRate, uint8_t root, uint8_t scale);
};

class Track {
private:
    EuclideanGenerator rhythm;
    NoteGenerator pitch;
    uint8_t currentStep;
public:
    void tick(const TrackParams& params); // 毎ステップ呼ばれる
};

class Clock {
    // タイマー割り込み管理
};

// --- Core 0 (UI) Classes ---
class InputManager {
public:
    void scan();
    bool isKeyPressed(Key key);
};

class DisplayController {
public:
    void drawTrackerUI(const TrackParams params[4], uint8_t cursorX, uint8_t cursorY);
};
```

## 4. 開発のフェーズ分け

1.  **Phase 1 (Core 1 単体テスト)**: UIなしで、Core 1上のみで `Track` と `EuclideanGenerator`, `NoteGenerator` を実装。シリアルモニタまたはUART MIDI出力でフレーズが正しく生成されているか確認する。
2.  **Phase 2 (Core 0 単体テスト)**: ジェネレーティブエンジンなしで、画面描画と9キーの入力によるカーソル移動、値の増減UIを構築する。
3.  **Phase 3 (結合・IPC)**: Core 0のUI操作を共有メモリ経由でCore 1に反映させる。
4.  **Phase 4 (Audio & Storage)**: プレビュー用シンセサイザー(I2S)の追加と、内蔵フラッシュメモリへのセーブ・ロード機能の実装。
