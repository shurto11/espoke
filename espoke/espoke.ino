/*
 * espoke — microSD の MP3 を Bluetooth イヤホンで鳴らし、Discord のメッセージをやりとりする
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble
 *       (Arduino IDE なら ツール → IP/Bluetooth Stack → "IPv4 + Bluetooth")
 *
 * 動作:
 *   1. Wi-Fi に接続し、microSD の MP3 を探して曲の一覧を作る
 *   2. Bluetooth イヤホン (A2DP sink) には、System の画面で Bluetooth を見ながら SW4 を押したときだけつなぐ
 *   3. シリアルに "sd" と打つと、microSD の MP3 をパス順に流し続ける
 *      BOOTSEL を押しても再生を始める
 *   4. LCD1602A の画面はホームから選ぶ。ホームの1行目は月日と時刻 (NTP で合わせた日本時間)、2行目で
 *      SW2 / SW3 で Music / Spotify / Discord / System を選び、SW4 で開く (Spotify は Wi-Fi につながっているときだけ出る)。
 *      Discord ではさらに Receive (受信) か Send (送信) を選ぶ。SW1 (か GP15 のスイッチ) で前の画面に戻る
 *   5. SW1〜SW4 (GP10〜13) の役目は開いている画面で変わる。SW1 はどこでも戻る (Send だけは長押しで戻る)
 *      ホーム・Discord: SW2 ← / SW3 → / SW4 開く
 *      Music: 再生の操作 (シリアルの pause / prev / next / shuffle と同じ)
 *        SW1 長押し 再生 / 一時停止 / SW2 前の曲 / SW3 次の曲 (シャッフル中はランダム) / SW4 シャッフルの入り切り
 *      Receive: SW2 古いメッセージ / SW3 新しいメッセージ
 *      Send: モールス信号を打ってアルファベットを入力する
 *        SW1 backspace (長押しで戻る) / SW2 トン / SW3 ツー / SW4 enter (符号を文字に確定。空なら空白、空白のあとなら送信)
 *      System: SW2 / SW3 で WiFi / Bluetooth / 稼働時間と空きヒープ を切り替える。Bluetooth で SW4 を押すとイヤホンにつなぐ
 *   6. RELAY_HOST の中継 (tools/discord_relay.py) に HTTPS でつなぎ、Discord のメッセージを受け取る。
 *      届いたら Receive の画面に切り替えて出す。中継が LCD の文字コード (英数字と半角カナ) に
 *      変換して送ってくるので、そのまま LCD に書ける。Send の画面で打った文は、同じ中継を通して Discord に送る
 *   7. ホームで Spotify を開くと Spotify モードになる。中継が、同じ PC で動かす go-librespot の音を MP3 にして
 *      WebSocket で流してくるので、SD の曲と同じく libmad でデコードしてイヤホンで鳴らす。モードの間は
 *      Spotify の画面だけを使い、Discord は受け取らない (届いた発言は、モードを抜けてつなぎ直すと届く)
 *        一覧: SW2 / SW3 でプレイリスト (先頭は Liked Songs) を選び、SW4 で曲選択へ。SW1 でモードを抜けてホームへ
 *        曲選択: 1行目に曲名 (鳴っている曲なら左端に ▶)、2行目にアーティストと番号。SW2 / SW3 で曲を選び、
 *                SW4 でその曲から再生する。SW4 長押しで、選んだ曲に関係なくシャッフルで再生する。SW1 で一覧へ
 *        一覧と曲選択の SW3 長押しで、再生中の画面へ
 *        再生中: SW1 長押し 再生 / 一時停止 / SW2 前の曲 / SW3 次の曲 / SW4 シャッフル / SW1 曲選択へ
 *
 * 音声フォーマット:
 *   MP3 (Layer III)。Pico 上で libmad (BackgroundAudio ライブラリ同梱) でデコードする。
 *   モノラル/ステレオ可。サンプリングレートは 44100 の整数分の1 (44100 / 22050 / 11025)
 *   のみ対応し、整数倍のサンプル&ホールドで 44100 ステレオへ引き伸ばして A2DP に流す。
 *   48000 など割り切れないレートは非対応 (リサンプラを積む余裕がないため)。
 *
 * 必要なライブラリ: LiquidCrystal I2C, BackgroundAudio (arduino-cli lib install で入れる)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <StackThunk.h>
#include <BluetoothAudio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
// BackgroundAudio に入っている libmad を使う。arduino-cli がライブラリを見つけられるよう、
// libmad を読み込んでいるこのヘッダ経由で取り込む (再生クラス自体は使わない)
#include <BackgroundAudioMP3.h>

#include "arduino_secrets.h"  // WIFI_SSID / WIFI_PASS / RELAY_HOST / RELAY_KEY
#include "relay_ca.h"         // RELAY_CA: 中継の証明書を確かめるルート CA

// 古い arduino_secrets.h でもビルドできるように。RELAY_HOST が空なら Discord は受け取らない
#ifndef RELAY_HOST
#define RELAY_HOST ""
#define RELAY_KEY  ""
#endif
#ifndef RELAY_PORT
#define RELAY_PORT 8443  // tailscale funnel が使えるのは 443 / 8443 / 10000
#endif
#ifndef SPOTIFY_PORT
#define SPOTIFY_PORT 10000  // Spotify は中継が TLS を受ける (Funnel の TCP 転送)。tools/discord_relay.py の「Spotify」の節
#endif

// --- 設定 ---
static const char *BT_LOCAL_NAME  = "espoke";  // イヤホン側に見える名前
static const char *BT_TARGET_NAME = "";        // 接続先の名前 (前方一致)。空なら最初の1台
static const char *BT_TARGET_ADDR = "a0:0c:e2:c6:1d:04";  // 空でなければスキャンせず直接繋ぐ
static const int    SCAN_SECONDS  = 8;
static const int    A2DP_RATE     = 44100;     // A2DPSource は 44100 か 48000 のみ
// A2DPSource はこの値を「バイト数」ではなく「16bit サンプル数」として扱い、2倍のバイトを確保する。
// 32768 で 64KB・約370ms 分。RAM は全部で 256KB しかなく、MP3 デコーダの作業領域 (約29KB) も要るので、
// 65536 (128KB) にすると a2dp.begin() が確保に失敗して Bluetooth が動かなくなる
static const size_t A2DP_BUFFER   = 32768;
static const bool   BOOTSEL_SKIP  = true;      // 再生中の BOOTSEL で次の曲へ送る
// 音量の目盛り (0〜100)。100 が等倍 (0dB) で、1 目盛りごとに VOLUME_TENTH_DB だけ下がる。0 は無音。
// 耳の感じ方に合わせて dB で刻む (振幅に比例させると、小さい音量のあたりで1目盛りの差が大きすぎた)
static const int    VOLUME_TENTH_DB = 6;       // 1 目盛りの大きさ (0.1dB 単位)。1〜100 で -59.4〜0dB
// 起動時の音量。イヤホン (a0:0c:e2:c6:1d:04) はつなぐたびに自分の音量を 80% に戻すので、
// その状態で耳で合わせた値 (-30dB)。細かい調整はイヤホンのボタンでする
static const int    VOLUME_DEFAULT = 50;
static const int    VOLUME_STEP    = 5;        // "vol +" / "vol -" で変える目盛り (3dB)

static const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)
static const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)
static const uint8_t LCD_COLS  = 16;
static const uint8_t LCD_ROWS  = 2;
static const uint8_t LCD_ADDR_DEFAULT = 0x27;
// 再生中は Music の画面の2行目を、帯域ごとの音の大きさの縦棒 (16 本、8 段) にする。
// 音量を掛ける前の音で見るので、vol を変えても棒の高さは変わらない
static const size_t        FFT_N       = 512;    // 1 回に見るフレーム数。44100Hz で 11.6ms、1 ビン 86Hz
static const uint32_t      BAR_HOP     = 1024;   // このフレーム数ごとに分析する (23ms)
static const unsigned long BAR_MS      = 80;     // 棒を描き直す間隔
static const float         BAR_TOP_DB  = 68.0f;  // 8 段 (いっぱい) になる大きさ。最大振幅の正弦波1本で約 78dB
static const float         BAR_STEP_DB = 4.5f;   // 1 段あたりの差。この3つは手元の曲で棒の平均が 3〜5 段になるように合わせた
static const float         BAR_TILT_DB = 0.5f;   // 1 本右へ行くごとに足す。曲は高い帯域ほど弱いので、右の棒も動くように
static const int     PIN_BUTTON = 15;  // GP15 (物理20番ピン)。もう片側は GND へ。SW1 と同じく前の画面に戻る
// SW1〜SW4。コメントは Send (モールス入力) の画面での役目 / Music の画面での役目。
// ホームなど項目を選ぶ画面では SW1 が戻る、SW2 が ←、SW3 が →、SW4 が開く
static const int     PIN_BACK   = 10;  // SW1 GP10 (物理14番ピン) backspace、長押しで戻る / 戻る、長押しで再生・一時停止
static const int     PIN_DOT    = 11;  // SW2 GP11 (物理15番ピン) トン / 前の曲
static const int     PIN_DASH   = 12;  // SW3 GP12 (物理16番ピン) ツー / 次の曲
static const int     PIN_ENTER  = 13;  // SW4 GP13 (物理17番ピン) enter / シャッフルの入り切り

// microSD (CK-40)。配線は docs/sd_music_parts.md の 3 章
static const int     PIN_SD_MISO = 16;  // GP16 (物理21番ピン) ⑦ DAT0
static const int     PIN_SD_SCK  = 18;  // GP18 (物理24番ピン) ⑤ CLK
static const int     PIN_SD_MOSI = 19;  // GP19 (物理25番ピン) ③ CMD
static const int     PIN_SD_CS   = 20;  // GP20 (物理26番ピン) ② DAT3/CS
static const size_t  SD_MAX_TRACKS = 300;  // 曲のパスは RAM に持つので上限を設ける (1曲 70 バイト前後)
static const int     SD_MAX_DEPTH  = 5;    // フォルダを潜る深さ
static const int     SD_REOPEN_MAX = 3;    // 1曲の途中で読めなくなったとき、カードを初期化し直して続ける回数

// Discord のメッセージ
static const size_t        MSG_MAX         = 8;    // 覚えておく数。中継は起動直後に直近のこの数だけ送ってくる
static const unsigned long MSG_SCROLL_MS   = 300;  // 16 文字に収まらない本文を1文字ずつ流す間隔
static const int           MSG_SCROLL_HOLD = 5;    // 本文の頭と末尾で止める長さ (MSG_SCROLL_MS 何回分か)

// Spotify。中継 (tools/discord_relay.py) が go-librespot の音を MP3 にして、WebSocket で流してくる
// MP3 の受信リング。SP_SEG ずつ、SP_SEGS_MAX 個まで取る (128kbps の 1 秒は 16KB)。ヒープは空きが 30KB あっても
// 細切れで、12KB を1つに取れなかった (実機)。小分けにすれば取れる。取るのは /spotify につないだ後にして、
// TLS が先に取れるようにする
static const size_t        SP_SEG        = 4096;
static const size_t        SP_SEGS_MAX   = 6;
static const uint32_t      SP_HEAP_SPARE = 8 * 1024;  // リングを取ったあとに残しておく空きヒープ (プレイリストの名前などに)
// 中継に伝える「まだ読んでいなくてよい量」(win)。Funnel の入口から dynabook までは往復 1 秒ほどかかる (実測) ので、
// 1 秒に win しか届かない。128kbps (16KB/秒) の 3 倍にしておく。曲送りのときに捨てる前の曲の量の上限でもある。
// TLS は中継が受けて 1KB ずつのレコードで送ってくるので、受信バッファ (RELAY_TLS_RX) より大きくてよい
static const int           SP_WIN        = 49152;
static const size_t        SP_DECODE_MIN = 2048;    // リングにこれだけあればデコードする (MP3 の1フレームは 128kbps で 418 バイト)
static const size_t        SP_CREDIT_STEP = 1024;   // これだけ読むたびに、読んだ量を中継に知らせる
// pollSpotify() で一度に読む上限。曲送りで前の曲 (win まで) を読み捨てるとき、全部読み終えるまで戻らないと
// イヤホンへ何も書けず、A2DP のバッファが空になってピーと鳴った (feedSilence() の説明)
static const size_t        SP_POLL_MAX   = 2048;
static const unsigned long SP_KEEPALIVE_MS = 30000; // 読んでいなくても、この間隔で知らせる (中継は 90 秒何も来なければ切る)
static const size_t        SP_LISTS_MAX  = 50;      // 覚えておくプレイリストの数
static const size_t        SP_TEXT_MAX   = 200;     // 音以外のフレームの中身の上限
// WebSocket の Sec-WebSocket-Key。中継も Funnel も値を確かめないので固定 ("espoke-spotify!!" の base64)
static const char *const   WS_KEY = "ZXNwb2tlLXNwb3RpZnkhIQ==";
// ------------

// .ino はビルド時に関数プロトタイプが先頭へ自動生成されるので、
// 引数や戻り値に使う型はここで定義しておく必要がある
enum PlayResult {
  PLAY_NONE,   // 中断要求なし (再生ループの中だけで使う)
  PLAY_DONE,   // 最後まで鳴らした
  PLAY_STOP,   // stop で止めた
  PLAY_SKIP,   // next / prev / rand / BOOTSEL で打ち切った
  PLAY_ERROR,  // SD の読み込みかデコードで失敗した
};

enum Step { STEP_NEXT, STEP_PREV, STEP_RAND };  // 次にどの曲へ進むか

// MP3 デコーダの作業領域 (約 29KB)
static const size_t MP3_IN_SIZE = 4096;  // 192kbps なら1フレーム約 630 バイト、320kbps でも約 1KB
struct Mp3Decoder {
  struct mad_stream stream;
  struct mad_frame  frame;
  struct mad_synth  synth;
  uint8_t           in[MP3_IN_SIZE + MAD_BUFFER_GUARD];
  bool              eof;  // ファイルを読み終え、末尾に MAD_BUFFER_GUARD 分の 0 を足した
  bool              readError;  // SD のファイルが末尾の手前で読めなくなった (カードが応答しない)
  File             *file; // 読む SD のファイル。nullptr なら Spotify の受信リングから読む
};

// 中継とつなぐ TLS のクライアント。BearSSL::WiFiClientSecure の中身 (Ctx) をそのまま使い、write() のたびに
// 相手の受け取り (TCP の ACK) を最大 300ms 待つのだけをやめる。Spotify では読んだ量を 1KB ごとに知らせるので、
// そのたびに往復の時間 (Funnel 越しで数十 ms) 止まると音が途切れる。データは lwIP に渡すところまでは書く
class RelayClient : public BearSSL::WiFiClientSecureCtx {
public:
  void flush() override {
    (void)BearSSL::WiFiClientSecureCtx::flush(1);
  }
};

// Discord のメッセージ1件。名前と本文は LCD の文字コード (英数字と半角カナ) のバイト列
struct Msg {
  String id;      // Discord のメッセージ ID。つなぎ直すとき、ここより後を送ってもらう
  String time;    // "HH:MM"
  String author;  // 10 バイトまで
  String text;    // 200 バイトまで
};

// 収まらない文字を左へ流して出す位置。MSG_SCROLL_MS ごとに1文字ずつ進め、頭と末尾で少し止める (marqStep())
struct Marquee {
  size_t pos;   // 何文字流したか
  int    hold;  // 端で止まっている残り回数
};

A2DPSource a2dp;
LiquidCrystal_I2C *lcd = nullptr;
static uint8_t lcdAddr = LCD_ADDR_DEFAULT;  // 棒を描くときは LiquidCrystal_I2C を通さず直接送る
static bool    lcdResyncOn = true;           // "lcd resync off" で、描き直すときの区切りの合わせ直しを止める (調べるとき用)
static const uint8_t LCD_PLAY_MARK = 6;      // 曲選択の画面で再生中の印 (▶) に使う自作文字。ほかの画面では棒の 7 段
static bool    lcdPlayMarkLoaded = false;    // 今 LCD_PLAY_MARK が ▶ になっているか

// 帯域の棒。writePcm() で鳴らす音をモノラルにして溜め、BAR_HOP ごとに FFT して段の高さを記録しておく。
// A2DP のバッファには 370ms 分ほど先の音まで入っているので、描くときは今イヤホンへ送り出している
// 所の記録を選ぶ (書いたときに描くと、棒が音より先に動く)
struct BarFrame {
  uint32_t frame;              // この分析の窓の終わりが、書いたフレームの通算で何番目か
  uint8_t  level[LCD_COLS];    // 0〜8 段
};
static const size_t BAR_HIST = 24;          // A2DP のバッファ1周分 (16 回) より多めに持つ
static BarFrame barHist[BAR_HIST];
static size_t   barHistHead = 0;            // 次に書く位置
static uint32_t barFrames = 0;              // A2DP に書いたフレーム数の通算 (無音も数える)
static uint32_t barNextHop = BAR_HOP;
static int16_t  barRing[FFT_N];             // 直近 FFT_N フレームのモノラル。barRingPos が一番古い
static uint32_t barRingPos = 0;
static uint8_t  barEdge[LCD_COLS + 1];      // 帯域の境目 (FFT のビン番号)。i 本目は barEdge[i]〜barEdge[i+1]-1
static uint8_t  barShown[LCD_COLS];         // LCD に出ている段。0xFF は描いていない (次に必ず書く)
static unsigned long lastBarDraw = 0;
static int16_t  fftRe[FFT_N], fftIm[FFT_N];
static int16_t  fftCos[FFT_N / 2], fftSin[FFT_N / 2];  // ひねり係数 (Q15)
static bool     playing = false;            // playSd の再生ループの中か

// A2DPSource::write() (arduino-pico 6.1.0) は、リングバッファの終わりをまたぐ書き込みで
// 後半にデータの先頭部分をもう一度書いてしまう (2回目の memcpy が buffer から読み直している)。
// 書き込みを常に A2DP_CHUNK サンプルずつにして、バッファの大きさをちょうど割り切らせ、
// 終わりをまたがないようにする。MP3 の1フレーム (2304 サンプル) をそのまま書くと、
// 約 0.37 秒ごとに音が壊れる
static const size_t A2DP_CHUNK = 2048;     // 16bit サンプル数 (1024 ステレオフレーム、4KB)
static_assert(A2DP_BUFFER % A2DP_CHUNK == 0, "A2DP_BUFFER must be a multiple of A2DP_CHUNK");
static int16_t pcmChunk[A2DP_CHUNK];
static size_t  pcmChunkLen = 0;            // pcmChunk に溜まっているサンプル数

// イヤホンには自分からはつながない。見つからない相手を探し続けると無線を取り合い、Wi-Fi がほとんど
// 通らなくなる (NTP や中継の TLS が失敗する)。System の Bluetooth で SW4 を押したときだけ1回つなぎにいく
static bool   btConnecting = false;        // SW4 か "bt" で頼まれた。loop() がつなぎにいき、済んだら false に戻す
static String btNote = "";                 // 最後につなぎにいって失敗した理由 ("BT timeout" など)。System に出す
static const unsigned long CONNECT_TIMEOUT_MS = 15000;  // ストリーム開始を待つ上限
static const unsigned long WIFI_RETRY_MS = 30000;      // Wi-Fi につながらないとき、つなぎ直す間隔
static const unsigned long HTTP_TIMEOUT_MS = 15000;    // 中継の応答を待つ上限
static const time_t        CLOCK_SET_AFTER = 1700000000; // time() がこれより前なら、時計はまだ NTP で合わせていない
static const char *const   CLOCK_TZ = "JST-9";         // ホームの時計のタイムゾーン (POSIX の TZ の書き方)
static const unsigned long RELAY_RETRY_MS = 30000;     // 中継へのつなぎ直しを試みる間隔。失敗が続くと倍ずつ延ばす
static const int           RELAY_BACKOFF_MAX = 4;      // 延ばすのは 2^4 倍 (8 分) まで
static const unsigned long RELAY_NTP_RETRY_MS = 10000; // 時刻合わせに失敗したときは、これだけ待ってすぐやり直す
static const unsigned long RELAY_IDLE_MS = 90000;      // これだけ何も届かなければ切れたとみなす (中継は 30 秒ごとに空行を送る)
static const size_t        RELAY_LINE_MAX = 300;       // 1行の上限。中継は本文を 200 バイトに切って送ってくる
// TLS の受信バッファ。既定の 16KB では Wi-Fi につないだ後の空きヒープ (30KB ほど) にほとんど残らない。
// Funnel の向こうの Go の TLS は MFLN (レコードを小さくする取り決め) に応じないが、実測では一番大きい
// レコードが証明書の 3430 バイトで、データは送った量が 128KB を超えるまで 1.2KB ほどに分けて送ってくる。
// 証明書のチェーンが長くなってつなげなくなったら (ssl のエラーが出る) 増やす
static const int           RELAY_TLS_RX = 4096;
// BearSSL は専用のスタック (arduino-pico の StackThunk) の上で動く。コアが確保するのは 6400 バイトだが、
// Let's Encrypt の P-384 の証明書を確かめると 6224 バイトまで使った (実測)。あふれると隣のヒープを壊すので、
// これだけの大きさのものに取り替える
static const size_t        RELAY_TLS_STACK = 8192;
static unsigned long lastWiFiTry = 0;
static bool   wifiEnabled = true;          // "wifi off" で false にすると、つなぎ直しもしない
static bool   wifiUp = false;              // 前の loop() で Wi-Fi につながっていたか。つながったときに知らせる
static int    volume = VOLUME_DEFAULT;     // 音量の目盛り。変えるときは applyVolume() を通す
static int32_t volumeGain = 0;             // writePcm() で振幅に掛ける倍率。65536 で等倍

// 連続再生の状態
static bool   autoPlay = false;            // 曲を続けて流しているか
static bool   paused = false;              // 一時停止中か。再生ループは無音を流しながら待つ
static String progressText = "";           // Music の画面の2行目に出している再生の進み具合
static Step   nextStep = STEP_NEXT;        // 次の曲の選び方
static bool   shuffle = false;             // シャッフル中か。「次の曲」をランダムな曲にする (stepToTake())
static int    playErrors = 0;              // 連続で失敗した回数
static unsigned long lastBootsel = 0;      // BOOTSEL を最後に見た時刻

// microSD の曲一覧 (パス順)
static bool                sdReady = false;
static std::vector<String> sdTracks;
static int                 sdIndex = -1;   // 最後に選んだ曲。-1 なら次は先頭から
// 再生のたびに確保すると、ヒープが細切れになったときに確保できなくなるので、最初から持っておく
static Mp3Decoder          mp3;

static String serialLine = "";             // 受信途中のコマンド

// LCD の画面。ホームで Music / Spotify / Discord / System を選んで開き、Discord ではさらに Receive (受信) か Send (送信) を選ぶ。
// SW1 (か GP15) で前の画面に戻る
enum Page { PAGE_HOME, PAGE_MUSIC, PAGE_DISCORD, PAGE_MSG, PAGE_INPUT, PAGE_SYSTEM, PAGE_SPOTIFY };
static const char *const PAGE_NAMES[] = {"home", "music", "discord", "receive", "send", "system", "spotify"};  // シリアルに出す名前
static int    page = PAGE_MUSIC;           // 起動中は Music の画面に状態を出し、setup() の最後でホームにする
static int    pageBeforeMsg = -1;          // 着信で Receive の画面に切り替える前の画面。戻るときはここへ
static String mainLines[2];                // Music の画面の内容。他の画面を見ている間も更新しておく
static unsigned long lastPageDraw = 0;
// 画面を描き直す間隔。中身が刻々と変わる画面は 1 秒ごと、それ以外も、LCD が化けたときに戻すためにこの間隔で
static const unsigned long PAGE_REDRAW_MS = 1000;
static const unsigned long PAGE_REFRESH_MS = 5000;

// ホームと Discord の画面で選ぶ項目。SW2 / SW3 で左右に動かし、SW4 で開く
struct MenuItem {
  const char *title;
  int         page;
};
static const MenuItem HOME_MENU[] = {{"Music", PAGE_MUSIC}, {"Spotify", PAGE_SPOTIFY}, {"Discord", PAGE_DISCORD},
                                     {"System", PAGE_SYSTEM}};
static const MenuItem DISCORD_MENU[] = {{"Receive", PAGE_MSG}, {"Send", PAGE_INPUT}};
static const int HOME_COUNT = sizeof(HOME_MENU) / sizeof(HOME_MENU[0]);
static const int DISCORD_COUNT = sizeof(DISCORD_MENU) / sizeof(DISCORD_MENU[0]);
static int    homeSel = 0;                 // ホームで選んでいる項目
static int    discordSel = 0;              // Discord の画面で選んでいる項目
// System の画面で見るもの。SW2 / SW3 で切り替える
enum SysView { SYS_WIFI, SYS_BT, SYS_INFO, SYS_COUNT };
static int    sysView = SYS_WIFI;

static const unsigned long DEBOUNCE_MS = 30;
static const unsigned long LONG_PRESS_MS = 600;  // SW1 をこれだけ押し続けると長押し (Music で再生・一時停止、Send で戻る)
static bool   buttonStable = HIGH;         // チャタリングを除いた状態 (INPUT_PULLUP なので離すと HIGH)
static bool   buttonRaw = HIGH;
static unsigned long buttonChanged = 0;

// SW1〜SW4。役目は開いている画面で変わる (onKey())
struct KeyState {
  int           pin;
  bool          raw;
  bool          stable;
  unsigned long changedAt;
  bool          holdable;  // 押したときに長押しのある役目だった。短く押したときの役目は離したときにする
  bool          longDone;  // 長押しした。離したときの役目はしない
};
enum { KEY_BACK, KEY_DOT, KEY_DASH, KEY_ENTER, KEY_COUNT };
// 基板のスイッチは SW1〜SW4 の4つだけなので、長押しで役目を足す (keyHasHold())。長押しのあるときは、
// 短く押したときの役目を、長押しと区別するため離したときにする (トン・ツーの SW2・SW3 はすぐ反応させたいので付けない)
static KeyState keys[KEY_COUNT] = {
  {PIN_BACK,  HIGH, HIGH, 0, false, false},
  {PIN_DOT,   HIGH, HIGH, 0, false, false},
  {PIN_DASH,  HIGH, HIGH, 0, false, false},
  {PIN_ENTER, HIGH, HIGH, 0, false, false},
};
// Music の画面で SW1〜SW4 を押したときに出すコマンド (SW1 は長押し)。シリアルから打ったのと同じに扱う
static const char *const PLAY_KEY_COMMANDS[KEY_COUNT] = {"pause", "prev", "next", "shuffle"};
static String keyCommand = "";             // スイッチで出して、まだ処理していないコマンド

struct Morse {
  char        ch;
  const char *code;
};
static const Morse MORSE_TABLE[] = {
  {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
  {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
  {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
  {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
  {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
  {'Z', "--.."},
  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
  {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
};
static const size_t MAX_CODE = 6;          // 1文字の符号は最長5つ (数字)
static const size_t MAX_TEXT = 64;
static String morseCode = "";              // 入力中の符号 ("-" と ".")
static String morseText = "";              // 確定した文章
static String morseNote = "";              // 送った結果。Send の画面の2行目に、符号を打ち始めるまで出す

// Discord の中継 (tools/discord_relay.py) から受け取ったメッセージ
static RelayClient *relay = nullptr;       // 最初につなぐときに作る (TLS のスタック 6.4KB を起動時から取らない)
static BearSSL::X509List *relayCA = nullptr;
static bool   relayOpen = false;           // メッセージの流れを受け取っている最中か
static bool   relayQuiet = false;          // 起動して最初の同期中。前からあった分なので着信として知らせない
static String relayStatus = "connecting";  // メッセージがまだ無いときに Receive の画面に出す
static unsigned long lastRelayTry = 0;     // 最後につなぎにいった時刻
static unsigned long relayRetryMs = 0;     // lastRelayTry からこれだけたったら、次につなぎにいく
static int    relayFailures = 0;           // 続けてつなげなかった回数
static unsigned long lastRelayData = 0;    // 最後に何か (空行を含む) 届いた時刻
static String relayLine = "";              // 受信途中の1行
static String relayLastId = "";            // 最後に受け取ったメッセージの ID
static String relayOutbox = "";            // Discord に送る文。次に中継につなぐとき POST で一緒に送る
static Msg    msgs[MSG_MAX];               // 古い順に msgCount 件
static size_t msgCount = 0;
static int    msgView = 0;                 // Receive の画面で見ているもの。0 が最新、1 がその1つ前
static Marquee msgMarq = {0, 0};          // 本文を流している位置
static unsigned long lastMsgScroll = 0;

// Spotify モード。その間は Spotify の画面だけを出し、中継には /spotify の WebSocket でつなぐ (Discord は受け取らない)
static bool   spMode = false;
static bool   spWs = false;                 // 中継との今の接続が /spotify の WebSocket か
// Spotify の画面の見え方。プレイリストの一覧 → 曲選択 → 再生中 の順に進み、SW1 で戻る
enum SpView { SP_LISTS, SP_TRACKS, SP_NOW };
static int    spView = SP_LISTS;
static std::vector<String> spLists;         // プレイリストの名前 (LCD の文字コード)。中継が L で送ってきた順
static int    spSel = 0;                    // 一覧で選んでいる番号
static int    spPlaying = -1;               // 再生中のプレイリストの番号。分からなければ -1
static int    spPlayingTrack = -1;          // 再生中の曲が、そのプレイリストの何番目か。分からなければ -1
// 曲選択の画面。曲名は中継に SP_PAGE 曲ずつ頼み (Q)、SP_NAMES 曲分だけ覚えておく (番号 % SP_NAMES の所に)
static const int SP_PAGE = 16;
static const int SP_NAMES = 32;
static int    spTrackList = -1;             // 曲選択の画面で見ているプレイリストの番号
static int    spTrackSel = 0;               // 選んでいる曲の番号 (0 から)
static int    spTrackTotal = -1;            // 曲の数。中継から届くまでは -1
static String spTrackNames[SP_NAMES];       // 曲名 (LCD の文字コード)
static String spTrackArtists[SP_NAMES];     // アーティスト (LCD の文字コード)
static int    spTrackNameAt[SP_NAMES];      // spTrackNames[k] が何番目の曲の名前か。-1 なら空
static Marquee spMarq[2] = {{0, 0}, {0, 0}};  // 曲選択の画面で、収まらない曲名 (1行目) とアーティスト (2行目) を流す位置
static Marquee spNowMarq = {0, 0};          // 再生中の画面で、収まらない曲名を流す位置
static unsigned long lastSpScroll = 0;
static int    spAskedPage[2] = {-1, -1};    // 最近頼んだ回の頭の番号。返事を待つ間に同じものを頼み直さない
static unsigned long spAskedAt[2] = {0, 0};
static String spTitle = "";                 // 今の曲名とアーティスト (LCD の文字コード)
static String spArtist = "";
static String spState = "";                 // 中継が知らせてきた go-librespot の状態 ("playing" / "paused" / "stopped")
static bool   spShuffle = false;
static bool   spPaused = false;             // 一時停止中。リングを読み進めずに無音を流す
static bool   spActive = false;             // 再生を頼んでいる。つなぎ直したら R で再開を頼む
static uint32_t spCmds = 0;                 // 中継へ送った操作 (P N B K R) の数。中継は S にそれまでに受け取った数を付けてくる
static bool   spWaitZ = false;              // 選曲・曲送りを頼んだ。Z が来るまでの音は前の曲なので捨てる
static bool   spReset = false;              // デコーダを初めからやり直す (リングを捨てたとき)
static bool   spBuffering = true;           // リングに溜まるのを待っている
static String spNote = "";                  // 中継が知らせてきたエラー。Spotify の画面の2行目に出す
static bool   sdStale = false;              // Spotify モードで曲の一覧と SD を手放した。SD から流す前に読み直す
static uint8_t *spSegs[SP_SEGS_MAX];        // MP3 の受信リング (SP_SEG ずつ)。/spotify につないだときに取り、モードを抜けたら返す
static size_t spRingSize = 0;               // 取れた大きさ (SP_SEG の倍数)。0 ならまだ取っていない
static size_t spRingHead = 0;               // 一番古いバイトの位置
static size_t spRingLen = 0;
static uint32_t spRead = 0;                 // 中継から読んだバイト数の累計 (中継に C で知らせる)
static uint32_t spReported = 0;             // 最後に知らせた spRead
static unsigned long spLastReport = 0;
// WebSocket のフレームを解く途中の状態
static uint8_t  wsHead[10];
static size_t   wsHeadLen = 0;
static bool     wsInFrame = false;          // 頭を読み終えて、中身を読んでいる
static uint8_t  wsOp = 0;
static uint32_t wsLeft = 0;                 // 中身の残りのバイト数
static char     wsKind = 0;                 // バイナリフレームの1バイト目 (種類)。0 ならまだ読んでいない
static String   wsText = "";                // 音以外のフレームの中身
static bool     wifiFast = false;           // "wifi fast" で、Spotify の間 Wi-Fi を常時受信にする (既定は省電力)

// ---------------- LCD ----------------

static void drawRow(uint8_t row, const String &text) {
  if (!lcd) return;
  String s = text.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s);
}

// 2行目に帯域の棒を出しているか。再生中で、一時停止していない間だけ
static bool barsOn() {
  return playing && !paused;
}

// Music の画面の1行を書き換える。他の画面を表示中なら覚えておくだけ。
// 2行目に棒を出している間は、文字は覚えておくだけにして、棒が消えたときに出す
static void printLine(uint8_t row, const String &text) {
  mainLines[row] = text;
  if (page == PAGE_MUSIC && !(row == 1 && barsOn())) drawRow(row, row == 0 ? musicTop() : text);
}

// 1行目の右端にシャッフルの印 (自作文字 7。⇄ の形) を付ける
static String withShuffleMark(const String &text, bool on) {
  if (!on) return text;
  String s = text.substring(0, LCD_COLS - 1);
  while (s.length() < LCD_COLS - 1) s += ' ';
  return s + '\x07';
}

// Music の画面の1行目。シャッフル中は右端に印を出す
static String musicTop() {
  return withShuffleMark(mainLines[0], shuffle);
}

static void status(const String &a, const String &b) {
  printLine(0, a);
  printLine(1, b);
  Serial.printf("[%s] %s\n", a.c_str(), b.c_str());
}

// 再生の進み具合を Music の画面の2行目に出す。一時停止中は後ろに "pause" を付ける
static void showProgress(const String &text) {
  progressText = text;
  printLine(1, paused ? text + " pause" : text);
}

static void setupLCD() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();

  uint8_t addr = 0;
  for (uint8_t a = 1; a < 127 && addr == 0; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) addr = a;
  }
  if (addr == 0) {
    Serial.println("LCD not found, using default 0x27");
    addr = LCD_ADDR_DEFAULT;
  } else {
    Serial.printf("LCD address: 0x%02X\n", addr);
  }
  lcdAddr = addr;

  lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  createLcdChars();
}

// 自作文字を LCD に作る。棒の 1〜7 段を自作文字 0〜6 (下から h 行を塗る)、シャッフルの印 (⇄) を 7 にする。
// 8 段は LCD に元からある全面黒 (0xFF)。自作文字は 8 つしか無いので、棒を出さない Spotify の曲選択の画面では、
// 6 を再生中の印 (▶) にする (lcdPlayMark())。lcdResync() のたびに作り直すので、LiquidCrystal_I2C を通さず
// 1 文字分ずつまとめて送る (8 文字で 30ms ほど)
static void createLcdChars() {
  static const uint8_t SHUFFLE_MARK[8] = {0x02, 0x1F, 0x02, 0x00, 0x08, 0x1F, 0x08, 0x00};
  static const uint8_t PLAY_MARK[8] = {0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10, 0x00};
  if (!lcd) return;
  lcdPlayMarkLoaded = lcdPlayMark();
  for (uint8_t c = 0; c < 8; c++) {
    Wire.beginTransmission(lcdAddr);
    lcdSendRaw(0x40 | (c << 3), false);  // CGRAM の c 文字目へ
    for (uint8_t r = 0; r < 8; r++) {
      uint8_t bits = (r >= 7 - c) ? 0x1F : 0x00;
      if (c == 7) bits = SHUFFLE_MARK[r];
      if (c == LCD_PLAY_MARK && lcdPlayMarkLoaded) bits = PLAY_MARK[r];
      lcdSendRaw(bits, true);
    }
    Wire.endTransmission();
  }
}

// 自作文字 LCD_PLAY_MARK を、棒の 7 段ではなく再生中の印 (▶) にしておく画面か
static bool lcdPlayMark() {
  return page == PAGE_SPOTIFY && spView == SP_TRACKS;
}

// ---------------- 帯域の棒 ----------------

// 固定小数点 (Q15) の FFT。各段で 1/2 にしてあふれないようにするので、結果は 1/FFT_N 倍になる
static void fft(int16_t *re, int16_t *im) {
  // ビット反転の順に並べ替える
  for (uint32_t i = 1, j = 0; i < FFT_N; i++) {
    uint32_t bit = FFT_N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (uint32_t len = 2; len <= FFT_N; len <<= 1) {
    const uint32_t half = len / 2, step = FFT_N / len;
    for (uint32_t i = 0; i < FFT_N; i += len) {
      for (uint32_t k = 0; k < half; k++) {
        // e^(-j 2π k/len)。16bit どうしの積の差なので 32bit に収まる
        const int32_t wr = fftCos[k * step], wi = -fftSin[k * step];
        const uint32_t a = i + k, b = a + half;
        const int32_t tr = (re[b] * wr - im[b] * wi) >> 15;
        const int32_t ti = (re[b] * wi + im[b] * wr) >> 15;
        re[b] = (re[a] - tr) >> 1;
        im[b] = (im[a] - ti) >> 1;
        re[a] = (re[a] + tr) >> 1;
        im[a] = (im[a] + ti) >> 1;
      }
    }
  }
}

// 窓関数 (Hann)。Q15。cos の表を使い回す (n と FFT_N - n で同じ値)
static int32_t hann(uint32_t n) {
  const uint32_t m = n < FFT_N / 2 ? n : FFT_N - n;
  return m == FFT_N / 2 ? 32767 : (32767 - fftCos[m]) >> 1;
}

// barRing の音を FFT して、16 本の段の高さを barHist に足す
static void analyzeBars() {
  for (uint32_t n = 0; n < FFT_N; n++) {
    fftRe[n] = (int16_t)((barRing[(barRingPos + n) & (FFT_N - 1)] * hann(n)) >> 15);
    fftIm[n] = 0;
  }
  fft(fftRe, fftIm);

  BarFrame &h = barHist[barHistHead];
  barHistHead = (barHistHead + 1) % BAR_HIST;
  h.frame = barFrames;
  for (int i = 0; i < LCD_COLS; i++) {
    // 帯域に入るビンの強さを足す。帯域は高いほど広いので、高い音も低い音と同じくらいの棒になる
    uint64_t power = 0;
    for (int k = barEdge[i]; k < barEdge[i + 1]; k++) {
      power += (uint32_t)(fftRe[k] * fftRe[k]) + (uint32_t)(fftIm[k] * fftIm[k]);
    }
    float db = 10.0f * log10f((float)power + 1.0f) + BAR_TILT_DB * i;
    int level = (int)((db - (BAR_TOP_DB - 8 * BAR_STEP_DB)) / BAR_STEP_DB);
    h.level[i] = (uint8_t)constrain(level, 0, 8);
  }
}

// A2DP に書く 1 フレーム分を、棒の分析に回す (音量を掛ける前のモノラル)
static inline void barPush(int16_t mono) {
  barRing[barRingPos] = mono;
  barRingPos = (barRingPos + 1) & (FFT_N - 1);
  if (++barFrames == barNextHop) {
    barNextHop += BAR_HOP;
    analyzeBars();
  }
}

// 無音を書いたことを記録する。その間は棒が 0 になる
static void barSilence(uint32_t frames) {
  barFrames += frames;
  barNextHop = barFrames + BAR_HOP;  // 次の分析までに barRing が新しい音で埋まる
  BarFrame &h = barHist[barHistHead];
  barHistHead = (barHistHead + 1) % BAR_HIST;
  h.frame = barFrames;
  memset(h.level, 0, sizeof(h.level));
}

// 次に描くときに 2行目を全部書き直させる (文字を出していた後など)
static void invalidateBars() {
  memset(barShown, 0xFF, sizeof(barShown));
}

// 再生を始めるときに呼ぶ。前の曲の記録を捨てて、棒を出し始める
static void startBars() {
  for (BarFrame &h : barHist) {
    h.frame = barFrames;  // 「今」より先の記録として扱われるので、新しい音が届くまで棒は 0
    memset(h.level, 0, sizeof(h.level));
  }
  invalidateBars();
  lastBarDraw = 0;
  playing = true;
}

static void setupBars() {
  for (uint32_t k = 0; k < FFT_N / 2; k++) {
    fftCos[k] = (int16_t)lroundf(cosf(2 * PI * k / FFT_N) * 32767);
    fftSin[k] = (int16_t)lroundf(sinf(2 * PI * k / FFT_N) * 32767);
  }
  // 86Hz (ビン 1) から 16kHz までを対数で 16 等分する。低い方は 1 ビンずつになる
  const float lo = 1, hi = 16000.0f * FFT_N / A2DP_RATE;
  int prev = 0;
  for (int i = 0; i <= LCD_COLS; i++) {
    int e = lroundf(lo * powf(hi / lo, (float)i / LCD_COLS));
    if (e <= prev) e = prev + 1;
    barEdge[i] = e;
    prev = e;
  }
  invalidateBars();

  uint32_t t0 = micros();
  analyzeBars();
  Serial.printf("bars: fft %lu us\n", (unsigned long)(micros() - t0));
}

// LCD に1文字ぶん送る (data なら文字、そうでなければ命令)。PCF8574 のつなぎは LiquidCrystal_I2C と同じで
// P0=RS, P2=E, P3=バックライト, P4〜P7=D4〜D7。RS とデータを出してから E を上げ下げし、下げたところで読ませる
static void lcdSendRaw(uint8_t b, bool data) {
  const uint8_t base = 0x08 | (data ? 0x01 : 0x00);
  Wire.write(base | (b & 0xF0));
  for (uint8_t nibble : {(uint8_t)(b & 0xF0), (uint8_t)(b << 4)}) {
    Wire.write(base | nibble | 0x04);
    Wire.write(base | nibble);
  }
}

// LCD の今の番地から1バイト読む (読むと番地は1つ進む)。RW (P1) を上げて D4〜D7 (P4〜P7) に 1 を書くと、
// PCF8574 の端子が手放されて LCD の出した値が読める。E を上げている間に上下 4bit を1回ずつ読む
static uint8_t lcdReadRaw() {
  const uint8_t base = 0xF0 | 0x08 | 0x02 | 0x01;  // D4〜D7 を手放す、バックライト、RW=読む、RS=データ
  uint8_t b = 0;
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(lcdAddr);
    Wire.write(base);
    Wire.write(base | 0x04);
    Wire.endTransmission();
    Wire.requestFrom(lcdAddr, (size_t)1);
    b = (b << 4) | ((Wire.available() ? Wire.read() : 0) >> 4);
    Wire.beginTransmission(lcdAddr);
    Wire.write(base);
    Wire.endTransmission();
  }
  return b;
}

// LCD の中身を読んでシリアルに出す ("lcd")。自作文字 0〜7 を絵で、2 行に書いてある文字コードを 16 進で
static void dumpLcd() {
  if (!lcd) return;
  uint8_t cg[64], dd[LCD_ROWS][LCD_COLS];
  Wire.beginTransmission(lcdAddr);
  lcdSendRaw(0x40, false);  // CGRAM の 0 番地へ
  Wire.endTransmission();
  for (uint8_t &v : cg) v = lcdReadRaw();
  for (uint8_t row = 0; row < LCD_ROWS; row++) {
    Wire.beginTransmission(lcdAddr);
    lcdSendRaw(0x80 | (row * 0x40), false);  // row 行目の頭へ
    Wire.endTransmission();
    for (uint8_t &v : dd[row]) v = lcdReadRaw();
  }
  Wire.beginTransmission(lcdAddr);
  Wire.write(0x08);  // RW を書く側に戻す
  Wire.endTransmission();

  Serial.println("lcd: CGRAM 0     1     2     3     4     5     6     7");
  for (int r = 0; r < 8; r++) {
    String line = "           ";
    for (int c = 0; c < 8; c++) {
      for (int bit = 4; bit >= 0; bit--) line += ((cg[c * 8 + r] >> bit) & 1) ? '#' : '.';
      line += ' ';
    }
    Serial.println(line);
  }
  for (uint8_t row = 0; row < LCD_ROWS; row++) {
    Serial.printf("lcd: row%u", row);
    for (uint8_t v : dd[row]) Serial.printf(" %02X", v);
    Serial.println();
  }
}

// "lcd" で中身を出し、"lcd chars" で自作文字を作り直す。"lcd resync off" / "on" で区切りの合わせ直しを止める / 戻す
static void lcdCommand(const String &cmd) {
  if (cmd == "lcd chars") {
    createLcdChars();
    Serial.println("lcd: chars rewritten");
  } else if (cmd.startsWith("lcd resync")) {
    lcdResyncOn = !cmd.endsWith("off");
    Serial.printf("lcd: resync %s\n", lcdResyncOn ? "on" : "off");
  } else {
    dumpLcd();
  }
}

// LCD の 4bit の区切りを合わせ直す。I2C の通信が1回化けると、LCD は 4bit の上下を取り違えたままになり、
// 画面全体がでたらめになって戻らない。HD44780 の初期化と同じく 0x3 を3回、0x2 を1回送ると、8bit / 4bit の
// どちらで、どこまで受け取っていても 4bit の頭に揃う。表示の中身は消えないので、このあと描き直す (5ms ほど)。
// LiquidCrystal_I2C の init() は 1 秒待つので使わない
static void lcdResync() {
  if (!lcd || !lcdResyncOn) return;
  bool first = true;
  for (uint8_t nibble : {0x30, 0x30, 0x30, 0x20}) {
    Wire.beginTransmission(lcdAddr);
    Wire.write(0x08 | nibble | 0x04);
    Wire.write(0x08 | nibble);
    Wire.endTransmission();
    // 1回目で、ずれて組み合わさった命令 (時間のかかる home なども) が終わるのを待つ
    delayMicroseconds(first ? 4500 : 150);
    first = false;
  }
  // 電源が揺れて LCD がリセットされていたときのために、表示の設定もし直す (LiquidCrystal_I2C の init() と同じ値)
  Wire.beginTransmission(lcdAddr);
  lcdSendRaw(0x28, false);  // 4bit、2 行、5x8 ドット
  lcdSendRaw(0x0C, false);  // 表示 ON、カーソルなし
  lcdSendRaw(0x06, false);  // 書くたびに右へ進む
  Wire.endTransmission();
  // この LCD は、上の合わせ直しで自作文字が壊れる (偶数番の文字に、隣の奇数番の中身が写る。7 段の棒が ⇄ になる)。
  // 合わせ直すたびに作り直す
  createLcdChars();
}

// 今イヤホンへ送り出している所の棒を 2行目に描く。LiquidCrystal_I2C は 4bit ごとに I2C の通信を
// 3 回するので 16 文字で 20ms ほど止まる。変わった所だけを1回の通信にまとめて送る (100kHz で最大 8ms 程度)
static void drawBars() {
  lastBarDraw = millis();
  if (!lcd || !(page == PAGE_MUSIC || (page == PAGE_SPOTIFY && spView == SP_NOW))) return;

  // A2DP のバッファと pcmChunk に残っている分だけ、今鳴っているのは書いた所より前
  const uint32_t queued = (A2DP_BUFFER - 1) - a2dp.availableForWrite() / 2 + pcmChunkLen;
  const uint32_t now = barFrames - queued / 2;
  const BarFrame *cur = nullptr;
  for (const BarFrame &h : barHist) {
    if ((int32_t)(h.frame - now) <= 0 && (!cur || (int32_t)(h.frame - cur->frame) > 0)) cur = &h;
  }

  static const uint8_t BAR_CHARS[9] = {' ', 0, 1, 2, 3, 4, 5, 6, 0xFF};
  uint8_t want[LCD_COLS];
  int first = -1, last = -1;
  for (int i = 0; i < LCD_COLS; i++) {
    uint8_t target = cur ? cur->level[i] : 0;
    // 上がるときはすぐ、下がるときは 1 段ずつ (棒がちらつかないように)
    want[i] = (barShown[i] != 0xFF && target < barShown[i]) ? barShown[i] - 1 : target;
    if (want[i] != barShown[i]) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0) return;

  Wire.beginTransmission(lcdAddr);
  lcdSendRaw(0x80 | (0x40 + first), false);  // 2行目の first 桁目へ
  for (int i = first; i <= last; i++) {
    lcdSendRaw(BAR_CHARS[want[i]], true);
    barShown[i] = want[i];
  }
  Wire.endTransmission();
}

// 符号を文字にする。表になければ 0
static char decodeMorse(const String &code) {
  for (const Morse &m : MORSE_TABLE) {
    if (code == m.code) return m.ch;
  }
  return 0;
}

// Receive の画面で見ている1件。msgCount が 0 のときは呼ばない
static const Msg &viewedMsg() {
  return msgs[msgCount - 1 - msgView];
}

// 頭から出し直す。頭で少し止めてから流し始める
static void marqReset(Marquee &m) {
  m.pos = 0;
  m.hold = MSG_SCROLL_HOLD;
}

// len 文字を width 文字の幅に出すとき、1文字流す。末尾まで来たら少し止めて、頭に戻ってまた止める。
// 収まるときと止めているときは動かさない。動かしたら true
static bool marqStep(Marquee &m, size_t len, size_t width) {
  if (len <= width) return false;
  if (m.hold > 0) {
    m.hold--;
    return false;
  }
  m.pos = (m.pos + width >= len) ? 0 : m.pos + 1;
  if (m.pos == 0 || m.pos + width >= len) m.hold = MSG_SCROLL_HOLD;
  return true;
}

// text を流した位置から width 文字分。足りなければ空白で埋める
static String marqText(const Marquee &m, const String &text, size_t width) {
  String s = text.length() > width ? text.substring(m.pos, m.pos + width) : text;
  while (s.length() < width) s += ' ';
  return s;
}

// Receive の画面の2行目に、本文を流した位置から書く
static void drawMsgText() {
  drawRow(1, viewedMsg().text.substring(msgMarq.pos));
}

// 本文を頭から出し直す
static void resetMsgScroll() {
  marqReset(msgMarq);
  lastMsgScroll = millis();
}

// 16 文字に収まらない本文を1文字ずつ流す
static void scrollMsg() {
  lastMsgScroll = millis();
  if (marqStep(msgMarq, viewedMsg().text.length(), LCD_COLS)) drawMsgText();  // 2行目だけ書き直す (16 文字で 20ms ほど)
}

// 選んでいる項目の名前を、左右の矢印で挟んで真ん中に出す。A00 の ROM では 0x7F が ←、0x7E が →
static String menuTitle(const char *title) {
  String s = "\x7f";
  for (int i = (LCD_COLS - 2 - (int)strlen(title)) / 2; i > 0; i--) s += ' ';
  s += title;
  while (s.length() < LCD_COLS - 1) s += ' ';
  return s + '\x7e';
}

// ホームの1行目。左に月日、右に時刻 (日本時間)。NTP で合わせるまでは数字の代わりに "--" を出す
static String clockLine() {
  time_t now = time(nullptr);
  struct tm t;
  if (now < CLOCK_SET_AFTER || !localtime_r(&now, &t)) return "--/--      --:--";
  char s[LCD_COLS + 1];
  snprintf(s, sizeof(s), "%02d/%02d      %02d:%02d", t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return s;
}

// ホームに出す項目か。Spotify は Wi-Fi につながっていて、中継があるときだけ
static bool homeItemShown(int i) {
  return HOME_MENU[i].page != PAGE_SPOTIFY || (RELAY_HOST[0] && WiFi.status() == WL_CONNECTED);
}

// Spotify の画面の2行目。一覧では選んでいる番号か、つながり具合やエラー
static String spListLine() {
  if (!relayOpen) return relayStatus;
  if (spNote.length()) return spNote;
  if (spLists.empty()) return "loading";
  String s = String(spSel + 1) + "/" + String(spLists.size());
  if (spSel == spPlaying) s += spPaused ? "  pause" : "  playing";
  return s;
}

// 曲選択で選んでいる曲が、鳴っている (か一時停止している) 曲か
static bool spSelIsPlaying() {
  return spTrackList == spPlaying && spTrackSel == spPlayingTrack && spState != "stopped";
}

// 曲選択の画面の1行目。鳴っている曲なら左端に印 (自作文字の ▶)、曲名 (収まらなければ流す)、
// シャッフル中は右端に印
static size_t spTopWidth() {
  return LCD_COLS - (spShuffle ? 1 : 0) - (spSelIsPlaying() ? 1 : 0);
}

static String spTrackTop() {
  const String *name = spTrackName(spTrackSel);
  String text = name ? *name : spTrackTotal == 0 ? String("(no tracks)") : String("loading...");
  String top = spSelIsPlaying() ? String((char)LCD_PLAY_MARK) : String();
  return withShuffleMark(top + marqText(spMarq[0], text, spTopWidth()), spShuffle);
}

// 再生中の画面の1行目。曲名 (収まらなければ流す)、シャッフル中は右端に印
static size_t spNowWidth() {
  return spShuffle ? LCD_COLS - 1 : LCD_COLS;
}

static String spNowTop() {
  return withShuffleMark(marqText(spNowMarq, spTitle.length() ? spTitle : String("Spotify"), spNowWidth()), spShuffle);
}

// 再生中の画面で、収まらない曲名を1文字ずつ流す (2行目の棒はそのまま)
static void scrollSpNow() {
  lastSpScroll = millis();
  if (marqStep(spNowMarq, spTitle.length(), spNowWidth())) drawRow(0, spNowTop());
}

// 再生中の画面に移る。曲名は頭から出す
static void spShowNow() {
  spView = SP_NOW;
  marqReset(spNowMarq);
  lastSpScroll = millis();
}

// 曲選択の画面の2行目。左にアーティスト (収まらなければ流す)、右に 番号/曲の数
static String spTrackNumber() {
  return String(spTrackSel + 1) + "/" + (spTrackTotal >= 0 ? String(spTrackTotal) : String("?"));
}

static size_t spArtistWidth() {
  return LCD_COLS - spTrackNumber().length() - 1;
}

static String spTrackLine() {
  if (!relayOpen) return relayStatus;
  if (spNote.length()) return spNote;
  const String *artist = spTrackArtist(spTrackSel);
  return marqText(spMarq[1], artist ? *artist : String(), spArtistWidth()) + " " + spTrackNumber();
}

// 曲名とアーティストを頭から出し直す (選ぶ曲が変わったときと、名前が届いたとき)
static void spResetTrackScroll() {
  marqReset(spMarq[0]);
  marqReset(spMarq[1]);
  lastSpScroll = millis();
}

// 曲選択の画面で、収まらない曲名とアーティストを1文字ずつ流す。動いた行だけ書き直す
static void scrollSpTrack() {
  lastSpScroll = millis();
  const String *name = spTrackName(spTrackSel);
  const String *artist = spTrackArtist(spTrackSel);
  if (name && marqStep(spMarq[0], name->length(), spTopWidth())) drawRow(0, spTrackTop());
  if (artist && relayOpen && !spNote.length() && marqStep(spMarq[1], artist->length(), spArtistWidth())) {
    drawRow(1, spTrackLine());
  }
}

// Spotify の画面の2行目。今の曲を見ているとき、棒を出していない間の様子
static String spNowLine() {
  if (!a2dp.connected()) return "no earphone";
  if (!relayOpen) return relayStatus;
  if (spNote.length()) return spNote;
  if (spPaused) return "pause " + spArtist;
  if (spState == "stopped" && !spWaitZ) return "stopped";
  if (spWaitZ || spBuffering) return "buffering...";
  return spArtist;
}

// Discord の画面の2行目に、選んでいる項目の今の様子を出す
static String pageSummary(int p) {
  switch (p) {
    case PAGE_MSG:
      // 最新の発言者と時刻。まだ無ければ中継とのつながり具合
      if (!msgCount) return relayStatus;
      return msgs[msgCount - 1].author + " " + msgs[msgCount - 1].time;
    case PAGE_INPUT:
      // 打ちかけの文 (収まらなければ末尾側)。無ければ送った結果
      if (morseText.length() > LCD_COLS) return morseText.substring(morseText.length() - LCD_COLS);
      if (morseText.length()) return morseText;
      return morseNote.length() ? morseNote : String("morse");
  }
  return "";
}

// 今の画面を描き直す。LCD の区切りも合わせ直すので、化けていても元に戻る。pollButton() が定期的に呼ぶ
static void drawPage() {
  lastPageDraw = millis();
  lcdResync();
  if (lcdPlayMark() != lcdPlayMarkLoaded) createLcdChars();  // "lcd resync off" で作り直していないとき
  switch (page) {
    case PAGE_HOME:
      if (!homeItemShown(homeSel)) homeSel = 0;  // Wi-Fi が切れて Spotify が消えた
      drawRow(0, clockLine());
      drawRow(1, menuTitle(HOME_MENU[homeSel].title));
      break;
    case PAGE_SPOTIFY:
      if (spView == SP_NOW) {
        drawRow(0, spNowTop());
        if (barsOn()) {
          invalidateBars();
          drawBars();
        } else {
          drawRow(1, spNowLine());
        }
      } else if (spView == SP_TRACKS) {
        drawRow(0, spTrackTop());
        drawRow(1, spTrackLine());
      } else {
        drawRow(0, spLists.empty() ? String("Spotify") : menuTitle(spLists[spSel].c_str()));
        drawRow(1, spListLine());
      }
      break;
    case PAGE_DISCORD:
      drawRow(0, menuTitle(DISCORD_MENU[discordSel].title));
      drawRow(1, pageSummary(DISCORD_MENU[discordSel].page));
      break;
    case PAGE_MSG:
      if (msgCount == 0) {
        drawRow(0, "MSG --");
        drawRow(1, relayStatus);
      } else {
        // 1行目は左に名前、右端に時刻
        const Msg &m = viewedMsg();
        String top = m.author;
        while (top.length() + m.time.length() < LCD_COLS) top += ' ';
        drawRow(0, top + m.time);
        drawMsgText();
      }
      break;
    case PAGE_INPUT: {
      // 1行目は末尾にカーソル代わりの '_' を付け、収まらなければ末尾側を見せる
      String top = morseText + "_";
      if (top.length() > LCD_COLS) top = top.substring(top.length() - LCD_COLS);
      drawRow(0, top);
      // 2行目は左に符号、右端に確定したときの文字。符号が無いときは送った結果
      String bottom = morseCode.length() ? morseCode : morseNote;
      if (morseCode.length()) {
        char ch = decodeMorse(morseCode);
        while (bottom.length() < LCD_COLS - 3) bottom += ' ';
        bottom += "= ";
        bottom += ch ? ch : '?';
      }
      drawRow(1, bottom);
      break;
    }
    case PAGE_SYSTEM:
      if (sysView == SYS_WIFI && WiFi.status() == WL_CONNECTED) {
        drawRow(0, "WiFi " + String(WiFi.RSSI()) + "dBm");
        drawRow(1, WiFi.localIP().toString());
      } else if (sysView == SYS_WIFI) {
        drawRow(0, "WiFi --");
        drawRow(1, WIFI_SSID);
      } else if (sysView == SYS_BT && a2dp.connected()) {
        // "aa:bb:cc:dd:ee:ff" は17文字で1行に収まらないのでコロンを抜く
        String addr = bd_addr_to_str(a2dp.getSinkAddress());
        addr.replace(":", "");
        drawRow(0, "BT connected");
        drawRow(1, addr);
      } else if (sysView == SYS_BT) {
        drawRow(0, "BT --");
        drawRow(1, btConnecting ? String("connecting...") : btNote.length() ? btNote : String("SW4: connect"));
      } else {
        unsigned long sec = millis() / 1000;
        char up[17];
        snprintf(up, sizeof(up), "up %luh%02lum%02lus", sec / 3600, sec / 60 % 60, sec % 60);
        drawRow(0, up);
        drawRow(1, "heap " + String(rp2040.getFreeHeap() / 1024) + "KB");
      }
      break;
    default:  // PAGE_MUSIC
      drawRow(0, musicTop());
      if (barsOn()) {
        invalidateBars();
        drawBars();
      } else {
        drawRow(1, mainLines[1]);
      }
      break;
  }
}

// 入力した文を Discord に送る。TLS の握手で数秒止まるので、ここでは頼むだけにして loop() か pollAbort() が送る
static void requestSend() {
  if (!RELAY_HOST[0]) {
    morseNote = "no RELAY_HOST";
  } else if (WiFi.status() != WL_CONNECTED) {
    morseNote = "send: no wifi";
  } else {
    relayOutbox = morseText;
    relayOutbox.trim();
    morseNote = "sending...";
  }
}

// 送った結果を Send の画面に出す。送れたら入力した文を消す。送れなければ残すので、enter でもう一度送れる。
// 送っていなければ (relayOutbox が空なら) 何もしない
static void sendDone(const String &result) {
  if (!relayOutbox.length()) return;
  Serial.printf("send: %s\n", result.c_str());
  relayOutbox = "";
  if (result == "ok") {
    morseText = "";
    morseNote = "sent";
  } else {
    morseNote = "send: " + result;
  }
  if (page == PAGE_INPUT) drawPage();
}

// 画面を開く。Receive の画面は最新のメッセージを頭から出す
static void openPage(int p) {
  page = p;
  pageBeforeMsg = -1;
  if (p == PAGE_MSG) {
    msgView = 0;
    resetMsgScroll();
  }
  Serial.printf("page %s\n", PAGE_NAMES[p]);
  drawPage();
}

// 前 (1つ上) の画面に戻る (SW1 か GP15)。着信で切り替わった Receive の画面からは、元の画面に戻る
static void goBack() {
  if (page == PAGE_SPOTIFY) onSpotifyKey(KEY_BACK);
  else if (page == PAGE_MSG && pageBeforeMsg >= 0) openPage(pageBeforeMsg);
  else if (page == PAGE_MSG || page == PAGE_INPUT) openPage(PAGE_DISCORD);
  else if (page != PAGE_HOME) openPage(PAGE_HOME);
}

// SW2 で1つ左 (前)、SW3 で1つ右 (次) へ。端まで行くと反対の端に回る
static int stepSel(int sel, int n, int id) {
  return (sel + (id == KEY_DASH ? 1 : n - 1)) % n;
}

static void onKey(int id) {
  if (page == PAGE_INPUT) {
    onMorseKey(id);
    return;
  }
  if (page == PAGE_SPOTIFY) {
    onSpotifyKey(id);
    return;
  }
  // SW1 はどの画面でも前の画面に戻る (Send だけは backspace で、戻るのは長押し)
  if (id == KEY_BACK) {
    goBack();
    return;
  }

  switch (page) {
    case PAGE_HOME:
    case PAGE_DISCORD: {
      // SW2 / SW3 で項目を選び、SW4 で開く
      bool home = page == PAGE_HOME;
      int &sel = home ? homeSel : discordSel;
      if (id == KEY_DOT || id == KEY_DASH) {
        // ホームでは出していない項目 (Wi-Fi が無いときの Spotify) を飛ばす。Music は必ず出ている
        do sel = stepSel(sel, home ? HOME_COUNT : DISCORD_COUNT, id);
        while (home && !homeItemShown(sel));
        drawPage();
        return;
      }
      if (id == KEY_ENTER) {
        int p = home ? HOME_MENU[sel].page : DISCORD_MENU[sel].page;
        if (p == PAGE_SPOTIFY) spotifyOn();
        else openPage(p);
      }
      break;
    }
    case PAGE_MUSIC:
      // SW2 前の曲 / SW3 次の曲 / SW4 シャッフルの入り切り
      keyCommand = PLAY_KEY_COMMANDS[id];
      Serial.printf("key: %s\n", keyCommand.c_str());
      break;
    case PAGE_MSG:
      // 曲の 前 / 次 の代わりに、メッセージの 古い方 / 新しい方 を見る
      if (id == KEY_DOT || id == KEY_DASH) {
        if (id == KEY_DOT && msgView + 1 < (int)msgCount) msgView++;
        if (id == KEY_DASH && msgView > 0) msgView--;
        resetMsgScroll();
        drawPage();
      }
      break;
    case PAGE_SYSTEM:
      if (id == KEY_DOT || id == KEY_DASH) {
        sysView = stepSel(sysView, SYS_COUNT, id);
        drawPage();
      }
      // Bluetooth を見ているときの SW4 でイヤホンにつなぐ。数秒〜15 秒止まるので、ここでは頼むだけにして loop() がつなぐ
      if (id == KEY_ENTER && sysView == SYS_BT && !a2dp.connected()) {
        btConnecting = true;
        drawPage();
      }
      break;
  }
}

// Spotify の画面のスイッチ。一覧でプレイリストを、曲選択で曲を選んで再生し、再生中は曲を送る (Music と同じ並び)。
// SW1 で1つ前の見え方に戻り、一覧からはモードを抜ける。曲選択の SW4 長押しは onHold()
static void onSpotifyKey(int id) {
  if (spView == SP_NOW) {
    switch (id) {
      case KEY_BACK:  // 鳴っているプレイリストの曲選択へ (鳴らし続ける)
        if (spPlaying >= 0 && spPlaying < (int)spLists.size()) spOpenTracks(spPlaying, std::max(spPlayingTrack, 0));
        else spView = SP_LISTS;
        drawPage();
        break;
      case KEY_DOT:
        spSkip(false);
        break;
      case KEY_DASH:
        spSkip(true);
        break;
      case KEY_ENTER:
        spSend("F");
        break;
    }
    return;
  }
  if (spView == SP_TRACKS) {
    switch (id) {
      case KEY_BACK:
        spView = SP_LISTS;
        drawPage();
        break;
      case KEY_DOT:
      case KEY_DASH:
        if (spTrackTotal > 0) {
          spTrackSel = stepSel(spTrackSel, spTrackTotal, id);
          spResetTrackScroll();
          spAskNames();
          drawPage();
        }
        break;
      case KEY_ENTER:  // 選んでいる曲から鳴らす
        if (spTrackTotal > 0) spPlayTrack(spTrackList, spTrackSel);
        break;
    }
    return;
  }
  switch (id) {
    case KEY_BACK:
      spotifyOff();
      break;
    case KEY_DOT:
    case KEY_DASH:
      if (!spLists.empty()) {
        spSel = stepSel(spSel, spLists.size(), id);
        drawPage();
      }
      break;
    case KEY_ENTER:  // 曲選択へ。鳴っているプレイリストなら、鳴っている曲を選んでおく
      if (!spLists.empty()) {
        spOpenTracks(spSel, spSel == spPlaying ? std::max(spPlayingTrack, 0) : 0);
        drawPage();
      }
      break;
  }
}

// 長押しのある役目か。SW1 はどの画面でも。Spotify では、曲選択の SW4 (ランダムに鳴らす) と、
// 一覧と曲選択の SW3 (再生中の画面へ)
static bool keyHasHold(int id) {
  if (id == KEY_BACK) return true;
  if (page != PAGE_SPOTIFY) return false;
  return (id == KEY_ENTER && spView == SP_TRACKS) || (id == KEY_DASH && spView != SP_NOW);
}

// 長押し。SW1 は、Music と Spotify では再生 / 一時停止、それ以外 (Send など) では前の画面に戻る。
// SW4 は Spotify の曲選択の画面で、選んでいる曲に関係なくシャッフルで鳴らす。
// SW3 は Spotify の一覧と曲選択の画面で、再生中の画面へ移る
static void onHold(int id) {
  if (id == KEY_ENTER) {
    if (page == PAGE_SPOTIFY && spView == SP_TRACKS) spShufflePlay(spTrackList);
    return;
  }
  if (id == KEY_DASH) {
    // 一覧と曲選択から、再生中の画面へ。何も鳴らしていなければ何もしない
    if (page == PAGE_SPOTIFY && spView != SP_NOW && (spActive || spState == "playing" || spState == "paused")) {
      spShowNow();
      drawPage();
    }
    return;
  }
  if (page == PAGE_SPOTIFY) {
    // まだ何も鳴らしていなければ、一覧で選んでいるものを鳴らす
    if (!spActive && !spPaused && spState != "paused") spPlay(spSel);
    else spTogglePause();
  } else if (page == PAGE_MUSIC) {
    keyCommand = PLAY_KEY_COMMANDS[KEY_BACK];
    Serial.printf("key: %s\n", keyCommand.c_str());
  } else {
    goBack();
  }
}

// Send の画面のスイッチ。モールスで文を打ち、enter で Discord に送る
static void onMorseKey(int id) {
  morseNote = "";
  switch (id) {
    case KEY_DASH:
    case KEY_DOT:
      if (morseCode.length() < MAX_CODE) morseCode += (id == KEY_DASH) ? '-' : '.';
      break;
    case KEY_BACK:
      if (morseCode.length()) {
        morseCode.remove(morseCode.length() - 1);
      } else if (morseText.length()) {
        morseText.remove(morseText.length() - 1);
      }
      break;
    case KEY_ENTER:
      if (morseCode.length()) {
        char ch = decodeMorse(morseCode);
        if (ch && morseText.length() < MAX_TEXT) morseText += ch;
        if (!ch) Serial.printf("unknown code: %s\n", morseCode.c_str());
        morseCode = "";
      } else if (morseText.endsWith(" ") || morseText.length() >= MAX_TEXT) {
        // 空の enter を2回続けると (1回目で空白が入る) 送る。いっぱいで空白が入らないときは1回で送る
        requestSend();
      } else if (morseText.length()) {
        morseText += ' ';
      }
      break;
  }
  Serial.printf("morse code=\"%s\" text=\"%s\"\n", morseCode.c_str(), morseText.c_str());
  drawPage();
}

// スイッチを見て、押されたらそれぞれの役目をする。再生中も呼ばれるので待たずに返す
static void pollButton() {
  for (int i = 0; i < KEY_COUNT; i++) {
    KeyState &k = keys[i];
    bool raw = digitalRead(k.pin);
    if (raw != k.raw) {
      k.raw = raw;
      k.changedAt = millis();
    } else if (raw != k.stable && millis() - k.changedAt >= DEBOUNCE_MS) {
      k.stable = raw;
      if (raw == LOW) {
        // 長押しのある役目なら、長押しと区別するため、短く押したときの役目は離したときにする
        k.holdable = keyHasHold(i);
        k.longDone = false;
        if (!k.holdable) onKey(i);
      } else if (k.holdable && !k.longDone) {
        onKey(i);
      }
    }
    // 長押しは離すのを待たずに動く。押している間に切り替わるので、手応えがある
    if (k.holdable && k.stable == LOW && !k.longDone && millis() - k.changedAt >= LONG_PRESS_MS) {
      k.longDone = true;
      onHold(i);
    }
  }

  bool raw = digitalRead(PIN_BUTTON);
  if (raw != buttonRaw) {
    buttonRaw = raw;
    buttonChanged = millis();
  } else if (raw != buttonStable && millis() - buttonChanged >= DEBOUNCE_MS) {
    buttonStable = raw;
    if (buttonStable == LOW) goBack();
  }

  if (page == PAGE_MSG && msgCount && millis() - lastMsgScroll >= MSG_SCROLL_MS) scrollMsg();
  if (page == PAGE_SPOTIFY && spView == SP_TRACKS && millis() - lastSpScroll >= MSG_SCROLL_MS) scrollSpTrack();
  if (page == PAGE_SPOTIFY && spView == SP_NOW && millis() - lastSpScroll >= MSG_SCROLL_MS) scrollSpNow();
  // Music と Spotify (曲名や棒は変わるたびに書いている)、Send (押したときに書いている)、流している本文は、描き直す間隔を空ける
  bool live = page != PAGE_MUSIC && page != PAGE_INPUT && page != PAGE_SPOTIFY && !(page == PAGE_MSG && msgCount);
  if (millis() - lastPageDraw >= (live ? PAGE_REDRAW_MS : PAGE_REFRESH_MS)) drawPage();
}

// ---------------- シリアル ----------------

// 改行まで溜まったら1行返す。溜まっていなければ空文字列。
// readStringUntil() は改行が来るまで最大1秒ブロックするので、
// 再生中に呼ぶとその間だけ音が途切れる。こちらは待たない
static String readLine() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length()) {
        String out = serialLine;
        serialLine = "";
        out.trim();
        return out;
      }
    } else if (serialLine.length() < 160) {
      serialLine += c;
    }
  }
  return String();
}

// 次に処理するコマンドを返す。スイッチで出したものがあれば、シリアルより先にそれを返す
static String nextCommand() {
  if (keyCommand.length()) {
    String out = keyCommand;
    keyCommand = "";
    return out;
  }
  return readLine();
}

// ---------------- Bluetooth ----------------

static void onConnect(void *, bool connected) {
  if (connected) {
    Serial.printf("A2DP connected: %s\n", bd_addr_to_str(a2dp.getSinkAddress()));
  } else {
    Serial.println("A2DP disconnected");
  }
}

// イヤホン側の音量 (AVRCP の Absolute Volume)。つないだときと、イヤホンのボタンで変えたときに届く。
// イヤホンは自分でこの音量を掛けるので、Pico の vol とは別に効く
static void onVolume(void *, int pct) {
  Serial.printf("earphone volume: %d%%\n", pct);
}

// "aa:bb:cc:dd:ee:ff" を 6バイトに変換する
static bool parseAddr(const char *str, uint8_t *out) {
  unsigned v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

// 接続要求を出し、ストリーム開始まで待つ
static bool connectTo(const uint8_t *addr, const char *label) {
  status("BT connecting", label);
  if (!a2dp.connect(addr)) {
    status("BT rejected", label);
    return false;
  }

  // connect() の戻り値は「要求が受理されたか」だけ。ここから先は非同期で
  // AVDTP シグナリング → SBC ネゴシエーション → ストリーム開始 と進み、
  // そこまで到達して初めて connected() が true になる
  unsigned long start = millis();
  while (!a2dp.connected() && millis() - start < CONNECT_TIMEOUT_MS) {
    pollButton();
    delay(10);
  }
  if (!a2dp.connected()) {
    status("BT timeout", label);
    return false;
  }
  feedSilence();  // ストリームはもう始まっているので、初期化されていないバッファが鳴る前に埋める
  status("BT ready", label);
  return true;
}

static bool scanAndConnect();

static bool findAndConnect() {
  // アドレスが分かっているならスキャンを飛ばす。8秒のインクワイアリを挟まない分、
  // 相手が応答できる状態のうちに繋ぎにいける
  if (BT_TARGET_ADDR[0] != '\0') {
    uint8_t addr[6];
    if (parseAddr(BT_TARGET_ADDR, addr)) {
      return connectTo(addr, BT_TARGET_ADDR);
    }
    Serial.printf("BT_TARGET_ADDR \"%s\" を解釈できない\n", BT_TARGET_ADDR);
  }
  return scanAndConnect();
}

static bool scanAndConnect() {
  status("BT scanning", String(SCAN_SECONDS) + "s ...");
  auto found = a2dp.scan(BluetoothHCI::speaker_cod, SCAN_SECONDS);

  if (found.empty()) {
    status("BT not found", "pairing mode?");
    return false;
  }

  int pick = -1;
  for (size_t i = 0; i < found.size(); i++) {
    Serial.printf("  [%u] %-24s %s  rssi=%d\n",
                  (unsigned)i, found[i].name(), found[i].addressString(), found[i].rssi());
    if (pick < 0 && (BT_TARGET_NAME[0] == '\0' ||
                     strncmp(found[i].name(), BT_TARGET_NAME, strlen(BT_TARGET_NAME)) == 0)) {
      pick = (int)i;
    }
  }
  if (pick < 0) {
    status("BT no match", BT_TARGET_NAME);
    return false;
  }

  return connectTo(found[pick].address(), found[pick].name());
}

// ---------------- 再生 ----------------

// 溜まった pcmChunk を A2DP に書く。丸ごと入る空きができるまで待つ
static void flushPcmChunk() {
  while (a2dp.connected() && (size_t)a2dp.availableForWrite() < sizeof(pcmChunk)) delay(1);
  if (a2dp.connected()) a2dp.write((const uint8_t *)pcmChunk, sizeof(pcmChunk));
  pcmChunkLen = 0;
}

// 鳴らすものがない間も、A2DP のバッファを無音で満たしておく。
// A2DPSource はバッファが空になると、読み出し位置の 256 サンプル (約 2.9ms) を送り直し続ける。
// そこにあるのは1周前の音か、確保しただけで初期化されていないメモリなので、
// 約 344Hz の繰り返しになり、音量に関係なく大きなピーという音が鳴る。
// 1周分を無音で書けば、その後にバッファが空になっても送り直されるのは無音になる。待たずに返す
static void feedSilence() {
  while (a2dp.connected() && (size_t)a2dp.availableForWrite() >= sizeof(pcmChunk)) {
    memset(pcmChunk + pcmChunkLen, 0, (A2DP_CHUNK - pcmChunkLen) * sizeof(int16_t));
    a2dp.write((const uint8_t *)pcmChunk, sizeof(pcmChunk));
    barSilence((A2DP_CHUNK - pcmChunkLen) / 2);
    pcmChunkLen = 0;
  }
}

// A2DP のバッファを1周分の無音で埋める (400ms ほど待つ)。この後しばらく何も書けなくても、
// 送り直されるのは無音になる。曲の端が残ったまま止まると、そこが繰り返し鳴ってしまう
static void fillSilence() {
  unsigned long start = millis();
  while (a2dp.connected() && millis() - start < 400) {
    feedSilence();
    delay(1);
  }
}

// 16bit PCM (channels 本のインターリーブ) に音量を掛け、rateMul 倍のサンプル&ホールドで
// 44100 ステレオに引き伸ばして A2DP に書く。A2DP_CHUNK に満たない端数は次に回す
static void writePcm(const int16_t *in, size_t frames, int channels, uint32_t rateMul) {
  const int32_t gain = volumeGain;  // 等倍以下しか掛けないので 16bit からはみ出さない
  for (size_t f = 0; f < frames; f++) {
    const int16_t inL = in[f * channels];
    const int16_t inR = (channels == 2) ? in[f * channels + 1] : inL;
    const int16_t mono = (int16_t)((inL + inR) >> 1);
    int16_t l = (int16_t)((inL * gain) >> 16);
    int16_t r = (int16_t)((inR * gain) >> 16);
    for (uint32_t k = 0; k < rateMul; k++) {
      barPush(mono);
      pcmChunk[pcmChunkLen++] = l;
      pcmChunk[pcmChunkLen++] = r;
      if (pcmChunkLen == A2DP_CHUNK) flushPcmChunk();
    }
  }
}

// 1曲の再生を締める。summary は LCD の2行目に出す再生量、slowCause は音が途切れたときの原因の説明
static void finishPlayback(PlayResult result, const String &summary, const char *slowCause) {
  // 曲を続けて流すときは無音で締めない。次の曲を取りに行っている間、
  // バッファに残った末尾がそのまま鳴り続けるので繋ぎが詰まる。
  // 打ち切ったときも同じ理由で、残りをそのまま鳴らし切らせる
  if (result == PLAY_DONE && !autoPlay) {
    // 端数を無音で埋めて書き出し、続けてバッファ1周分 (約370ms) の無音を流す
    memset(pcmChunk + pcmChunkLen, 0, (A2DP_CHUNK - pcmChunkLen) * sizeof(int16_t));
    flushPcmChunk();
    for (size_t i = 0; i < A2DP_BUFFER / A2DP_CHUNK && a2dp.connected(); i++) {
      memset(pcmChunk, 0, sizeof(pcmChunk));
      flushPcmChunk();
    }
  }

  if (a2dp.getUnderflow()) {
    Serial.printf("warning: audio underflow (%s)\n", slowCause);
  }
  // 曲を送ったり止めたりしたら一時停止も解く
  if (result == PLAY_SKIP || result == PLAY_STOP) paused = false;
  playing = false;  // 2行目を棒から文字に戻す
  showProgress(summary + " " +
               (result == PLAY_DONE ? "done" : result == PLAY_STOP ? "stop" :
                result == PLAY_SKIP ? "skip" : "error"));
}

// "wifi off" で切断し、つなぎ直しもやめる。"wifi on" で loop() からのつなぎ直しを再開する
// (SD から流している間は、止めるまでつながない)。WiFi.disconnect() は Wi-Fi の接続を切るだけで、
// 無線チップは止めないので Bluetooth はそのまま使える。
// "wifi fast" / "wifi save" で、Spotify の間の Wi-Fi を常時受信 / 省電力にする (既定は省電力)。
// 常時受信は速いが Bluetooth と無線を取り合う (SD から流すときは、いつも省電力にする)
static void setWiFi(const String &cmd) {
  if (cmd == "wifi fast" || cmd == "wifi save") {
    wifiFast = cmd == "wifi fast";
    if (spMode && WiFi.status() == WL_CONNECTED) {
      if (wifiFast) WiFi.noLowPowerMode();
      else WiFi.defaultLowPowerMode();
    }
    Serial.printf("WiFi: %s during spotify\n", wifiFast ? "always on" : "power save");
  } else if (cmd == "wifi off") {
    wifiEnabled = false;
    relayClose("wifi off");
    WiFi.disconnect();
    status("WiFi off", "");
  } else if (cmd == "wifi on") {
    wifiEnabled = true;
    lastWiFiTry = millis() - WIFI_RETRY_MS;  // 次の loop() ですぐつなぎにいく
    Serial.println("WiFi: reconnect when not playing");
  } else {
    Serial.printf("wifi=%s connected=%d spotify=%s\n", wifiEnabled ? "on" : "off", WiFi.status() == WL_CONNECTED,
                  wifiFast ? "fast" : "save");
  }
}

// 音量の目盛りを決め、振幅に掛ける倍率を求めておく。再生中でも次に書くサンプルから効く
static void applyVolume(int v) {
  volume = constrain(v, 0, 100);
  float db = -(100 - volume) * VOLUME_TENTH_DB / 10.0f;
  volumeGain = volume ? (int32_t)lroundf(powf(10.0f, db / 20.0f) * 65536.0f) : 0;
}

// "vol 40" で目盛りを 40 に、"vol +" / "vol -" で VOLUME_STEP ずつ変える。"vol" だけなら今の値を出す
static void setVolume(const String &cmd) {
  String arg = cmd.substring(3);
  arg.trim();
  int v = volume;
  if (arg == "+") v += VOLUME_STEP;
  else if (arg == "-") v -= VOLUME_STEP;
  else if (arg.length()) v = arg.toInt();
  applyVolume(v);
  if (volume == 0) {
    Serial.println("volume=0 (mute)");
  } else {
    int tenths = (100 - volume) * VOLUME_TENTH_DB;
    Serial.printf("volume=%d (-%d.%ddB)\n", volume, tenths / 10, tenths % 10);
  }
}

// シャッフルを入り切りする。Music の画面の1行目の右端に印を出す。再生中でも次に選ぶ曲から効く
static void toggleShuffle() {
  shuffle = !shuffle;
  Serial.printf("shuffle=%s\n", shuffle ? "on" : "off");
  if (page == PAGE_MUSIC) drawRow(0, musicTop());
}

// 一時停止と再開を切り替える。再生ループは、止めている間は曲を読み進めずに無音を流す
static void togglePause() {
  paused = !paused;
  Serial.println(paused ? "paused" : "resumed");
  invalidateBars();  // 再開したら、一時停止中に出していた文字を棒で上書きする
  showProgress(progressText);
}

// 再生中にコマンドやボタンで中断されたかを見る。中断がなければ PLAY_NONE
static PlayResult pollAbort() {
  pollButton();
  if (spMode) return PLAY_STOP;  // ホームで Spotify を開いた
  pollRelay();
  // enter で送ると決めた文は、曲の途中でも待たずに送る。TLS の握手の間 (4〜8 秒) は無音になり、その後は続きから鳴る
  if (relayOutbox.length() && WiFi.status() == WL_CONNECTED) {
    fillSilence();
    relayConnect();
  }

  String cmd = nextCommand();
  if (cmd.length()) {
    if (cmd == "pause") {
      togglePause();
      return PLAY_NONE;
    }
    if (cmd == "stop") return PLAY_STOP;
    if (cmd == "next") { nextStep = STEP_NEXT; return PLAY_SKIP; }
    if (cmd == "prev") { nextStep = STEP_PREV; return PLAY_SKIP; }
    if (cmd == "rand") { nextStep = STEP_RAND; return PLAY_SKIP; }
    if (cmd == "shuffle") {
      toggleShuffle();
      return PLAY_NONE;
    }
    if (cmd.startsWith("wifi")) {
      setWiFi(cmd);
      return PLAY_NONE;
    }
    if (cmd.startsWith("vol")) {
      setVolume(cmd);
      return PLAY_NONE;
    }
    if (cmd.startsWith("lcd")) {
      lcdCommand(cmd);
      return PLAY_NONE;
    }
    Serial.println("再生中に使えるのは pause / stop / next / prev / rand / shuffle / vol [0-100|+|-] / wifi on / wifi off / lcd");
  }

  // BOOTSEL の読み取りは一瞬フラッシュと割り込みを止めるので、頻繁には見に行かない
  if (BOOTSEL_SKIP && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      nextStep = STEP_NEXT;
      return PLAY_SKIP;
    }
  }
  return PLAY_NONE;
}

// ---------------- microSD ----------------

static bool isMp3(const String &name) {
  // macOS がコピー時に作る "._曲名.mp3" は中身が MP3 ではないので除く
  if (name.startsWith("._")) return false;
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

static void scanSdDir(const String &dir, int depth) {
  File d = SD.open(dir);
  if (!d) return;
  while (sdTracks.size() < SD_MAX_TRACKS) {
    File e = d.openNextFile();
    if (!e) break;
    String name = e.name();
    String path = dir + (dir.endsWith("/") ? "" : "/") + name;
    if (e.isDirectory()) {
      if (depth + 1 < SD_MAX_DEPTH && name != "System Volume Information") scanSdDir(path, depth + 1);
    } else if (isMp3(name)) {
      sdTracks.push_back(path);
    }
    e.close();
  }
  d.close();
}

// カードを初期化し直す。曲の一覧はそのまま
static bool sdBegin() {
  SD.end();
  SPI.setRX(PIN_SD_MISO);
  SPI.setSCK(PIN_SD_SCK);
  SPI.setTX(PIN_SD_MOSI);
  sdReady = SD.begin(PIN_SD_CS);
  return sdReady;
}

// カードが応答しなくなった (開けない・曲の途中で読めない) とき、初期化し直して path を開き直し、読めていたところへ進める。
// 再生中に一度こうなると、カードは初期化し直すまで何も受け付けない (15 分ほど流すと起きた)。
// 直ったかどうかをログに出す。直るなら一瞬のこと (電源の瞬断や接触)、直らないならカードが落ちたまま
static bool sdReopen(File &f, const String &path, const char *why) {
  uint32_t at = f ? f.position() : 0;
  f.close();
  unsigned long t0 = millis();
  bool ok = sdBegin();
  if (ok) {
    f = SD.open(path, FILE_READ);
    ok = f && f.seek(at);
  }
  Serial.printf("SD: %s at %luKB, uptime %lus -> reinit %s (%lu ms, heap %luKB)\n", why, (unsigned long)(at / 1024),
                t0 / 1000, ok ? "ok" : "failed", millis() - t0, (unsigned long)(rp2040.getFreeHeap() / 1024));
  return ok;
}

// カードを (挿し直されていても) 初期化し直し、MP3 の一覧を作り直す
static bool setupSD() {
  sdStale = false;
  sdBegin();
  sdTracks.clear();
  if (!sdReady) {
    Serial.println("SD: no card");
    return false;
  }
  scanSdDir("/", 0);
  // アルバムのフォルダ順・曲番号順に並ぶよう、大文字小文字を区別せずパスで並べる
  std::sort(sdTracks.begin(), sdTracks.end(), [](const String &a, const String &b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  if (sdIndex >= (int)sdTracks.size()) sdIndex = -1;
  Serial.printf("SD: %u tracks%s\n", (unsigned)sdTracks.size(),
                sdTracks.size() >= SD_MAX_TRACKS ? " (limit reached)" : "");
  return !sdTracks.empty();
}

// 実際に進む先。シャッフル中は「次の曲」をランダムな曲にする。SW3 (next)・曲の終わり・BOOTSEL・再生の開始のどれでも
static Step stepToTake() {
  return (shuffle && nextStep == STEP_NEXT) ? STEP_RAND : nextStep;
}

// stepToTake() に従って次の曲を選ぶ
static int pickSdTrack() {
  int n = sdTracks.size();
  switch (stepToTake()) {
    case STEP_PREV:
      return sdIndex <= 0 ? n - 1 : sdIndex - 1;
    case STEP_RAND: {
      if (sdIndex < 0 || n == 1) return random(n);
      int r = random(n - 1);
      return r >= sdIndex ? r + 1 : r;  // 今の曲以外から選ぶ
    }
    default:
      return (sdIndex + 1) % n;
  }
}

// パスの最後の要素から拡張子を除いて曲名にする
static String trackTitle(const String &path) {
  String name = path.substring(path.lastIndexOf('/') + 1);
  int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

// ID3v2 タグ (曲名や画像が入っている) を読み飛ばし、最初の MP3 フレームの位置へ進める
static void skipId3(File &f) {
  uint8_t h[10];
  if (f.read(h, 10) == 10 && memcmp(h, "ID3", 3) == 0) {
    // サイズは 7bit ずつの 4 バイト (syncsafe)。フッタ付きならさらに 10 バイト
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    f.seek(10 + size + ((h[5] & 0x10) ? 10 : 0));
  } else {
    f.seek(0);
  }
}

// デコーダを初期化する。file が nullptr なら Spotify の受信リングから読む
static void mp3Start(Mp3Decoder *m, File *file) {
  mad_stream_init(&m->stream);
  mad_frame_init(&m->frame);
  mad_synth_init(&m->synth);
  m->eof = false;
  m->readError = false;
  m->file = file;
}

static void mp3End(Mp3Decoder *m) {
  mad_synth_finish(&m->synth);
  mad_frame_finish(&m->frame);
  mad_stream_finish(&m->stream);
  m->file = nullptr;
}

// 未使用の入力を前に詰め、空いたところを SD のファイルか Spotify の受信リングから埋める。ファイルを読み終えたら
// MAD_BUFFER_GUARD 分の 0 を足して最後のフレームまで出させる。もう足すものがなければ false。
// リングが空なのは曲の終わりではない (届くのを待つ) ので、eof にせず stream.error を BUFLEN のままにして false を返す。
// SD のファイルが末尾の手前で読めなくなったときも同じにして readError を立てる (playSd() が開き直して続きから読む)
static bool mp3Refill(Mp3Decoder *m) {
  if (m->eof) return false;
  size_t keep = 0;
  if (m->stream.next_frame) {
    keep = m->stream.bufend - m->stream.next_frame;
    memmove(m->in, m->stream.next_frame, keep);
  }
  int n = m->file ? m->file->read(m->in + keep, MP3_IN_SIZE - keep) : (int)ringGet(m->in + keep, MP3_IN_SIZE - keep);
  if (n <= 0 && (!m->file || m->file->position() < m->file->size())) {
    mad_stream_buffer(&m->stream, m->in, keep);
    m->stream.error = MAD_ERROR_BUFLEN;
    m->readError = m->file != nullptr;
    return false;
  }
  if (n <= 0) {
    memset(m->in + keep, 0, MAD_BUFFER_GUARD);
    n = MAD_BUFFER_GUARD;
    m->eof = true;
  }
  mad_stream_buffer(&m->stream, m->in, keep + n);
  m->stream.error = MAD_ERROR_NONE;
  return true;
}

// 1フレーム分デコードして m->synth.pcm に出す。曲の終わりか、続けられないエラーなら false
// (Spotify では、リングが空になったときも false。そのときは stream.error が MAD_ERROR_BUFLEN。
// SD が途中で読めなくなったときも false で、readError が立つ)
static bool mp3Decode(Mp3Decoder *m) {
  while (true) {
    if (m->stream.buffer == nullptr || m->stream.error == MAD_ERROR_BUFLEN) {
      if (!mp3Refill(m)) return false;
    }
    if (mad_frame_decode(&m->frame, &m->stream) == 0) break;
    if (MAD_RECOVERABLE(m->stream.error)) continue;  // 壊れたフレームやタグの残りは飛ばして同期を取り直す
    if (m->stream.error != MAD_ERROR_BUFLEN) {
      Serial.printf("mp3 decode error 0x%04x\n", m->stream.error);
      return false;
    }
  }
  mad_synth_frame(&m->synth, &m->frame);
  return true;
}

static PlayResult playSd(int index) {
  if (!a2dp.connected()) {
    status("no earphone", "cannot play");
    return PLAY_ERROR;
  }
  const String &path = sdTracks[index];
  String title = trackTitle(path);

  File f = SD.open(path, FILE_READ);
  if (!f && !sdReopen(f, path, "open failed")) {
    status("sd open error", title);
    return PLAY_ERROR;
  }
  // arduino-pico は Wi-Fi をつなぐと省電力を切る (常時受信)。そのままだと同じチップの
  // Bluetooth と無線を取り合い、音がブツブツ途切れたので、SD から流す間は省電力にする
  if (WiFi.status() == WL_CONNECTED) WiFi.defaultLowPowerMode();

  Mp3Decoder *m = &mp3;
  mp3Start(m, &f);
  skipId3(f);

  Serial.printf("playing %s (%d/%u) — %lu bytes\n", path.c_str(), index + 1,
                (unsigned)sdTracks.size(), (unsigned long)f.size());
  printLine(0, title);

  PlayResult result = PLAY_DONE;
  uint32_t rateMul = 1;
  uint32_t frames = 0;
  uint64_t decodeUs = 0;
  uint32_t underflows = 0;
  uint32_t maxGapUs = 0;            // loop が1周するのにかかった最長時間。長ければ何かに止められている
  uint32_t lastIter = micros();
  int reopens = 0;
  unsigned long lastLcd = 0;
  unsigned long started = millis();
  // A2DP ストリームは接続直後から流れ続けているので、再生を始めるまでの無音区間でも
  // underflow フラグが立つ。再生直前に一度読み捨てて、以降の取りこぼしだけを見る
  a2dp.getUnderflow();
  startBars();

  while (a2dp.connected()) {
    uint32_t now = micros();
    if (now - lastIter > maxGapUs) maxGapUs = now - lastIter;
    lastIter = now;

    PlayResult abort = pollAbort();
    if (abort != PLAY_NONE) {
      result = abort;
      break;
    }

    // 一時停止中はデコードせずに待つ。A2DP には無音を流しておく (feedSilence() の説明を参照)
    if (paused) {
      feedSilence();
      delay(1);
      continue;
    }

    // MP3 の1フレームは最大 1152 サンプル。引き伸ばした後のステレオ分と、
    // pcmChunk に溜まっている端数を書き出す分 (最大1チャンク) が入るまで待つ
    if ((size_t)a2dp.availableForWrite() < 1152 * 4 * rateMul + sizeof(pcmChunk)) {
      delay(1);
      continue;
    }

    uint32_t t0 = micros();
    if (!mp3Decode(m)) {
      // 曲の途中で読めなくなった。カードを初期化し直して続きから読む (デコーダには読めた分が残っている)
      if (m->readError && reopens < SD_REOPEN_MAX) {
        reopens++;
        if (sdReopen(f, path, "read error")) {
          m->readError = false;
          continue;
        }
      }
      if (!m->eof) result = PLAY_ERROR;
      break;
    }
    decodeUs += micros() - t0;
    frames++;

    struct mad_pcm &pcm = m->synth.pcm;
    rateMul = pcm.samplerate ? (uint32_t)A2DP_RATE / pcm.samplerate : 0;
    if (rateMul == 0 || rateMul > 4 || rateMul * pcm.samplerate != (uint32_t)A2DP_RATE) {
      Serial.printf("unsupported: %uHz\n", pcm.samplerate);
      status("unsupported", String(pcm.samplerate) + "Hz");
      result = PLAY_ERROR;
      break;
    }
    // libmad はモノラルのとき左だけ埋めるので、右にも写してステレオとして扱う
    if (pcm.channels == 1) {
      for (size_t i = 0; i < pcm.length; i++) pcm.samplesX[i][1] = pcm.samplesX[i][0];
    }
    writePcm(&pcm.samplesX[0][0], pcm.length, 2, rateMul);

    if (millis() - lastBarDraw >= BAR_MS) drawBars();

    if (millis() - lastLcd >= 500) {
      lastLcd = millis();
      showProgress("sd " + String((uint32_t)((uint64_t)f.position() * 100 / f.size())) + "%");
      // 途切れたらその場で出す。耳で聞いた途切れとログを突き合わせられるように。
      // 最初の1秒は、止まっていて空だったバッファが溜まるまでの分なので数えない
      if (a2dp.getUnderflow() && millis() - started >= 1000) {
        underflows++;
        Serial.printf("underflow at %lus (wifi %s)\n", (millis() - started) / 1000,
                      WiFi.status() == WL_CONNECTED ? "connected" : "off");
      }
    }
  }

  uint32_t pos = f.position();
  mp3End(m);
  f.close();

  // 1フレームは 1152 サンプル = 約 26ms。デコードにその何割を使ったかで CPU の余裕が分かる
  if (frames) {
    uint32_t avgUs = decodeUs / frames;
    Serial.printf("mp3: %lu frames, decode avg %lu us/frame (%lu%% of real time), "
                  "longest loop %lu ms, underflow %lu times, sd reopen %d times\n",
                  (unsigned long)frames, (unsigned long)avgUs,
                  (unsigned long)(avgUs * 100 / (1152UL * 1000000 / A2DP_RATE)),
                  (unsigned long)(maxGapUs / 1000), (unsigned long)underflows, reopens);
  }
  finishPlayback(result, String(pos / 1024) + "KB", "SD の読み込みかデコードが追いついていない");
  return result;
}

// ---------------- Discord ----------------

// BearSSL の専用スタックを RELAY_TLS_STACK バイトのものに取り替える。コアの StackThunk は 6400 バイト固定で、
// 変える設定がない。スタックの場所は呼ぶたびに stack_thunk_top から読まれるので、BearSSL の外でなら取り替えられる。
// 使った量が分かるよう、全体を目印 (0xdeadbeef) で塗っておく
static void growTlsStack() {
  uint32_t *stack = (uint32_t *)malloc(RELAY_TLS_STACK);
  if (!stack) return;  // 確保できなければコアのものを使い続ける
  for (size_t i = 0; i < RELAY_TLS_STACK / 4; i++) stack[i] = 0xdeadbeef;
  free(stack_thunk_ptr);
  stack_thunk_ptr = stack;
  stack_thunk_top = stack + RELAY_TLS_STACK / 4 - 1;
}

// BearSSL のスタックをこれまでに最大何バイト使ったか。底から目印が残っている所を数える
static size_t tlsStackUsed() {
  size_t words = RELAY_TLS_STACK / 4, i = 0;
  while (i < words && stack_thunk_ptr[i] == 0xdeadbeef) i++;
  return (words - i) * 4;
}

// 中継とのつながりを切る。つなぎ直しは loop() が relayRetryMs たってから試みる
static void relayClose(const String &why) {
  if (relayOpen) Serial.printf("relay: closed (%s)\n", why.c_str());
  relayOpen = false;
  spWs = false;
  relayStatus = why;
  if (relay) relay->stop();  // TLS の受信バッファなどを返す
}

// 中継につなげなかった。中継が落ちていると握手のタイムアウト (15 秒) まで待つので、
// 続くたびに間隔を倍にし、曲の合間が何度も止まらないようにする
static void relayFailed() {
  relayFailures++;
  relayRetryMs = RELAY_RETRY_MS << std::min(relayFailures, RELAY_BACKOFF_MAX);
}

// 中継につなぎ、メッセージの流れを受け取り始める。TLS の握手に 4〜8 秒かかり (P-384 の証明書を確かめるのが重い)、
// A2DP のバッファ (370ms) では持たないので、再生ループの中 (pollAbort) からは送る文があるときしか呼ばない。
// loop() が、止まっている間か曲の合間に呼ぶ。送る文 (relayOutbox) があれば、POST で一緒に送る。
// Spotify モードでは /spotify に WebSocket でつなぐ
static void relayConnect() {
  lastRelayTry = millis();
  if (spMode) spotifyPrepare();
  if (!relay) {
    relayCA = new BearSSL::X509List(RELAY_CA);
    relay = new RelayClient();  // ここで BearSSL のスタックが確保される
    relay->setTrustAnchors(relayCA);
    relay->setBufferSizes(RELAY_TLS_RX, 512);
    relay->setTimeout(HTTP_TIMEOUT_MS);  // 送るときは、中継が Discord に送り終えるまで返事が来ない
    growTlsStack();
  }
  // 送るときは、受け取っている最中でもつなぎ直す。POST の返事がそのまま新しい流れになる
  if (relayOpen) relayClose("sending");
  // 証明書の期限を確かめるのに時計が要る。起動してまだ合わせていなければ NTP で合わせる
  if (time(nullptr) < CLOCK_SET_AFTER) {
    NTP.begin("ntp.nict.jp", "pool.ntp.org");
    if (!NTP.waitSet(5000)) {
      // Wi-Fi につないだ直後は、DNS や最初の問い合わせが通らないことがある。中継の失敗とは数えず、
      // 間隔も延ばさずに少し待ってやり直す (NTP.begin() からやり直すので、名前も引き直す)
      Serial.println("relay: ntp failed");
      relayStatus = "ntp failed";
      relayRetryMs = RELAY_NTP_RETRY_MS;
      sendDone(relayStatus);
      return;
    }
  }

  // TLS は受信バッファ (RELAY_TLS_RX)、BearSSL のスタック (RELAY_TLS_STACK)、コンテキストなどを使う。足りているかをログで見る
  uint32_t heap = rp2040.getFreeHeap();
  unsigned long t0 = millis();
  if (!relay->connect(RELAY_HOST, spMode ? SPOTIFY_PORT : RELAY_PORT)) {
    char err[64] = "";
    int code = relay->getLastSSLError(err, sizeof(err));
    Serial.printf("relay: connect failed (ssl %d %s), heap %luKB\n", code, err, (unsigned long)(heap / 1024));
    relayClose("relay error");
    relayFailed();
    sendDone(relayStatus);
    return;
  }
  Serial.printf("relay: tls %lu ms, heap %luKB -> %luKB, bearssl stack %u/%u bytes\n", millis() - t0,
                (unsigned long)(heap / 1024), (unsigned long)(rp2040.getFreeHeap() / 1024),
                (unsigned)tlsStackUsed(), (unsigned)RELAY_TLS_STACK);

  // HTTP/1.0 で頼む。Funnel の手前の Go のプロキシが chunked にせず、届いた分をそのまま流してくる。
  // 送る文があれば POST の本文にする。中継は Discord に送ってから、GET と同じく流し始める。
  // Spotify は WebSocket に切り替えてもらう (切り替えは HTTP/1.1 にしかない)
  if (spMode) {
    relay->printf("GET /spotify?win=%d HTTP/1.1\r\nHost: %s\r\nX-Key: %s\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                  SP_WIN, RELAY_HOST, RELAY_KEY, WS_KEY);
  } else if (relayOutbox.length()) {
    relay->printf("POST /stream?after=%s HTTP/1.0\r\nHost: %s\r\nX-Key: %s\r\n"
                  "Content-Type: text/plain\r\nContent-Length: %u\r\n\r\n%s",
                  relayLastId.c_str(), RELAY_HOST, RELAY_KEY, relayOutbox.length(), relayOutbox.c_str());
  } else {
    relay->printf("GET /stream?after=%s HTTP/1.0\r\nHost: %s\r\nX-Key: %s\r\n\r\n",
                  relayLastId.c_str(), RELAY_HOST, RELAY_KEY);
  }
  String head = relay->readStringUntil('\n');  // "HTTP/1.0 200 OK" (Spotify は "HTTP/1.1 101 Switching Protocols")
  head.trim();
  if (head.substring(9, 12) != (spMode ? "101" : "200")) {
    Serial.printf("relay: \"%s\"\n", head.c_str());
    relayClose(head.length() ? "relay " + head.substring(9, 12) : String("relay no reply"));
    relayFailed();
    sendDone(relayStatus);
    return;
  }
  // ヘッダは空行 ("\r") で終わる。送ったときは、その結果 ("ok" か理由) が X-Sent に入っている
  String sent = "no answer";
  while (relay->connected()) {
    String h = relay->readStringUntil('\n');
    if (h.length() <= 1) break;
    if (h.substring(0, 7).equalsIgnoreCase("X-Sent:")) {
      sent = h.substring(7);
      sent.trim();
    }
  }

  relayOpen = true;
  relayFailures = 0;
  relayRetryMs = RELAY_RETRY_MS;  // 切れたら、つないでから RELAY_RETRY_MS たっていればすぐつなぎ直す
  lastRelayData = millis();
  if (spMode) {
    spotifyConnected();
    return;
  }
  relayQuiet = relayLastId.length() == 0;
  relayLine = "";
  relayStatus = "no message";
  Serial.printf("relay: connected (after=%s)\n", relayLastId.c_str());
  sendDone(sent);
}

// 中継から届いた1行を処理する。"ID TAB HH:MM TAB 名前 TAB 本文"。空行は生存確認で、
// つないだ直後の空行は「つなぐ前からあった分はここまで」の印
static void onRelayLine(const String &line) {
  if (!line.length()) {
    relayQuiet = false;
    return;
  }
  int a = line.indexOf('\t');
  int b = line.indexOf('\t', a + 1);
  int c = line.indexOf('\t', b + 1);
  if (a < 0 || b < 0 || c < 0) {
    Serial.printf("relay: bad line (%u bytes)\n", line.length());
    return;
  }

  // いっぱいなら一番古いものを捨てる
  if (msgCount == MSG_MAX) {
    for (size_t i = 1; i < MSG_MAX; i++) msgs[i - 1] = std::move(msgs[i]);
    msgCount--;
  }
  Msg &m = msgs[msgCount++];
  m.id = line.substring(0, a);
  m.time = line.substring(a + 1, b);
  m.author = line.substring(b + 1, c);
  m.text = line.substring(c + 1);
  relayLastId = m.id;
  // 名前と本文は半角カナのバイト列なので、シリアルでは化ける
  Serial.printf("relay: %s %s %s\n", m.id.c_str(), m.time.c_str(), m.author.c_str());

  if (relayQuiet) {
    // 起動前からあった分。表示は切り替えず、Receive の画面の中身だけ新しくしておく。
    // 古いものを見ていたら同じものを見続け、最新を見ていたら新しく来た方を頭から出す
    if (msgView > 0) msgView = std::min<int>(msgView + 1, msgCount - 1);
    else resetMsgScroll();
    if (page == PAGE_MSG) drawPage();
    return;
  }

  // 着信。Receive の画面に切り替えて最新を出す。戻るときに元の画面へ戻れるよう覚えておく
  if (page != PAGE_MSG) pageBeforeMsg = page;
  page = PAGE_MSG;
  msgView = 0;
  resetMsgScroll();
  drawPage();
}

// 届いた分だけ読んで1行ずつ処理する。待たないので、再生中も pollAbort() から呼べる
static void pollRelay() {
  if (spWs) {
    pollSpotify();
    return;
  }
  if (!relayOpen || !relay) return;
  uint8_t buf[64];
  int n;
  while ((n = relay->read(buf, sizeof(buf))) > 0) {
    lastRelayData = millis();
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        onRelayLine(relayLine);
        relayLine = "";
      } else if (buf[i] != '\r' && relayLine.length() < RELAY_LINE_MAX) {
        relayLine += (char)buf[i];
      }
    }
  }
  if (n < 0 || !relay->connected()) {
    relayClose("relay lost");
  } else if (millis() - lastRelayData >= RELAY_IDLE_MS) {
    relayClose("relay silent");
  }
}

// ---------------- Spotify ----------------
//
// 中継 (tools/discord_relay.py) と WebSocket で1本つなぎ、go-librespot の音を MP3 で受け取って鳴らす。
// フレームはすべてバイナリで、先頭1バイトが種類 (中身は LCD の文字コード):
//   中継から A MP3 / L 番号 TAB 総数 TAB プレイリスト名 / T 曲名 TAB アーティスト /
//            S 状態 TAB シャッフル 0/1 TAB 再生中のプレイリストの番号 TAB 中継が受け取った操作の数 /
//            Z 選曲・曲送りの切れ目 / H 生存確認 / E エラー
//   中継へ   C 読んだ量の累計 / P 番号 / N 次 / B 前 / K 一時停止 / R 再開 / F シャッフル
// 中継は、Pico がまだ読んでいない量を win 以下に抑えて送ってくる。Pico はリングに空きがあるだけしか読まないので、
// 流れてくる速さは Pico が鳴らす速さになる

static size_t ringFree() {
  return spRingSize - spRingLen;
}


// 受信リングに足す。呼ぶ側が空きを確かめてから読んでいるので、あふれることは無い
static void ringPut(const uint8_t *p, size_t n) {
  n = std::min(n, ringFree());
  size_t pos = (spRingHead + spRingLen) % spRingSize;
  spRingLen += n;
  while (n) {
    size_t off = pos % SP_SEG;
    size_t k = std::min(n, SP_SEG - off);
    memcpy(spSegs[pos / SP_SEG] + off, p, k);
    p += k;
    n -= k;
    pos = (pos + k) % spRingSize;
  }
}

static size_t ringGet(uint8_t *p, size_t n) {
  n = std::min(n, spRingLen);
  size_t pos = spRingHead;
  for (size_t left = n; left;) {
    size_t off = pos % SP_SEG;
    size_t k = std::min(left, SP_SEG - off);
    memcpy(p, spSegs[pos / SP_SEG] + off, k);
    p += k;
    left -= k;
    pos = (pos + k) % spRingSize;
  }
  spRingHead = pos;
  spRingLen -= n;
  return n;
}

static void spDraw() {
  if (page == PAGE_SPOTIFY) drawPage();
}

// 中継へ1つ送る。WebSocket の決まりで、クライアントから送るフレームはマスクする
static void spSend(const String &cmd) {
  if (!relayOpen || !spWs || cmd.length() > 125) return;
  if (strchr("PUNBKR", cmd[0])) spCmds++;  // 状態を変える操作は数えておく (S の説明)
  uint8_t f[6 + 125];
  uint32_t mask = rp2040.hwrand32();
  f[0] = 0x82;  // FIN + バイナリ
  f[1] = 0x80 | cmd.length();
  memcpy(f + 2, &mask, 4);
  for (size_t i = 0; i < cmd.length(); i++) f[6 + i] = cmd[i] ^ f[2 + (i & 3)];
  relay->write(f, 6 + cmd.length());
}

// 読んだ量を中継に知らせる。中継はこれを見て、続きを送ってよいかを決める
static void spReportRead() {
  spReported = spRead;
  spLastReport = millis();
  spSend("C" + String(spRead));
}

// 選曲・曲送りを頼んだ。リングに残っている前の曲を捨て、Z が来るまで届く分も捨てる
static void spFlush() {
  spRingHead = spRingLen = 0;
  spWaitZ = true;
  spReset = true;
  spBuffering = true;
  spPaused = false;
}

// 鳴らし始めるのを頼み、再生中の見え方に移る。title は曲名 (T) が届くまで出しておく名前
static void spStart(const String &cmd, const String &title) {
  if (!a2dp.connected()) {
    spNote = "no earphone";
    spDraw();
    return;
  }
  spSend(cmd);
  spFlush();
  spActive = true;
  spNote = "";
  spTitle = title;
  spArtist = "";
  spShowNow();
  drawPage();
}

// n 番目のプレイリストを頭から鳴らす
static void spPlay(int n) {
  if (n < 0 || n >= (int)spLists.size()) return;
  spSel = n;
  spStart("P" + String(n), spLists[n]);
}

// n 番目のプレイリストを i 番目の曲から鳴らす (そのあとはシャッフルの入り切りのとおり)
static void spPlayTrack(int n, int i) {
  if (n < 0 || n >= (int)spLists.size()) return;
  const String *name = spTrackName(i);
  spStart("P" + String(n) + ":" + String(i), name ? *name : spLists[n]);
}

// n 番目のプレイリストをシャッフルで鳴らす。どの曲から鳴らすかもランダム (中継がシャッフルを入れてから鳴らす)
static void spShufflePlay(int n) {
  if (n < 0 || n >= (int)spLists.size()) return;
  spShuffle = true;
  spStart("U" + String(n), spLists[n]);
}

// i 番目の曲の名前。覚えていなければ nullptr
static const String *spTrackName(int i) {
  if (i < 0) return nullptr;
  int k = i % SP_NAMES;
  return spTrackNameAt[k] == i ? &spTrackNames[k] : nullptr;
}

// i 番目の曲のアーティスト。覚えていなければ nullptr
static const String *spTrackArtist(int i) {
  if (i < 0) return nullptr;
  int k = i % SP_NAMES;
  return spTrackNameAt[k] == i ? &spTrackArtists[k] : nullptr;
}

// 覚えている曲名を捨てる (別のプレイリストを開いたときと、モードを抜けたとき)
static void spForgetTracks() {
  for (int k = 0; k < SP_NAMES; k++) {
    spTrackNames[k] = String();
    spTrackArtists[k] = String();
    spTrackNameAt[k] = -1;
  }
  spTrackTotal = -1;
  spAskedPage[0] = spAskedPage[1] = -1;
}

// start 番目から SP_PAGE 曲の名前を中継に頼む。覚えているか、頼んで 5 秒たっていなければ頼まない
static void spAskPage(int start) {
  if (start < 0 || (spTrackTotal >= 0 && start >= spTrackTotal)) return;
  int last = spTrackTotal >= 0 ? std::min(start + SP_PAGE, spTrackTotal) - 1 : start;
  if (spTrackName(start) && spTrackName(last)) return;
  int slot = 0;
  for (int r = 0; r < 2; r++) {
    if (spAskedPage[r] == start && millis() - spAskedAt[r] < 5000) return;
    if (spAskedAt[r] < spAskedAt[slot]) slot = r;
  }
  spAskedPage[slot] = start;
  spAskedAt[slot] = millis();
  spSend("Q" + String(spTrackList) + ":" + String(start));
}

// 選んでいる曲の名前を頼む。回の端の近くなら、進んでいる向きの次の回も先に頼んでおく
// (音を流している間は、返事が届くまで音の後ろで数秒待つので)
static void spAskNames() {
  int start = spTrackSel / SP_PAGE * SP_PAGE;
  spAskPage(start);
  int offset = spTrackSel - start;
  if (offset >= SP_PAGE - 4) spAskPage(start + SP_PAGE);
  if (offset < 4) spAskPage(start > 0 ? start - SP_PAGE : (spTrackTotal - 1) / SP_PAGE * SP_PAGE);
}

// n 番目のプレイリストの曲選択を開く。cursor の曲を選んでおく
static void spOpenTracks(int n, int cursor) {
  if (n != spTrackList) {
    spForgetTracks();
    spTrackList = n;
  }
  spTrackSel = (spTrackTotal > 0 && cursor >= spTrackTotal) ? 0 : cursor;
  spView = SP_TRACKS;
  spResetTrackScroll();
  spAskNames();
}

static void spSkip(bool next) {
  spSend(next ? "N" : "B");
  spFlush();
  spDraw();
}

static void spTogglePause() {
  spPaused = !spPaused;
  if (!spPaused) spActive = true;
  spSend(spPaused ? "K" : "R");
  Serial.println(spPaused ? "spotify: paused" : "spotify: resumed");
  spDraw();
}

// Spotify モードに入る。SD の再生は止める (再生ループの中なら pollAbort() が抜ける)。
// Discord の流れを切り、loop() が /spotify につなぎ直す。Wi-Fi につながっていなければ入らない
static void spotifyOn() {
  if (spMode) {
    openPage(PAGE_SPOTIFY);
    return;
  }
  if (!RELAY_HOST[0] || WiFi.status() != WL_CONNECTED) {
    Serial.println("spotify: no wifi");
    return;
  }
  spMode = true;
  autoPlay = false;
  paused = false;
  spLists.clear();
  spSel = 0;
  spPlaying = spPlayingTrack = spTrackList = -1;
  spForgetTracks();
  spView = SP_LISTS;
  spTitle = spArtist = spState = spNote = "";
  spShuffle = spPaused = spActive = spWaitZ = false;
  relayClose("spotify");
  relayStatus = "connecting...";
  relayFailures = 0;
  relayRetryMs = 0;
  Serial.println("spotify: on");
  openPage(PAGE_SPOTIFY);
}

// Spotify モードを抜けてホームへ戻る。後片付け (接続を切る、リングを返す) は loop() の spotifyCleanup() がする
// (再生ループの中から呼ばれても、使っているリングを消さないように)
static void spotifyOff() {
  if (!spMode) return;
  spMode = false;
  Serial.println("spotify: off");
  openPage(PAGE_HOME);
}

// /spotify につなぐ前に、SD の曲の一覧と SD のライブラリを手放してヒープを空ける (SD から流す前に読み直す)
static void spotifyPrepare() {
  if (sdStale) return;
  sdTracks.clear();
  sdTracks.shrink_to_fit();
  SD.end();
  sdStale = true;
}

// /spotify につないだあとで受信リングを取る。空きヒープを SP_HEAP_SPARE 残せる分だけ
static bool spotifyAllocRing() {
  size_t count = spRingSize / SP_SEG;
  while (count < SP_SEGS_MAX && rp2040.getFreeHeap() >= SP_SEG + SP_HEAP_SPARE) {
    uint8_t *seg = (uint8_t *)malloc(SP_SEG);
    if (!seg) break;
    spSegs[count++] = seg;
  }
  spRingSize = count * SP_SEG;
  Serial.printf("spotify: ring %u bytes, heap %luKB\n", (unsigned)spRingSize, (unsigned long)(rp2040.getFreeHeap() / 1024));
  return spRingSize >= 2 * SP_SEG;  // 8KB はほしい
}

// Spotify モードを抜けたあとの後片付け。中継は切られると go-librespot を止める。Discord にはすぐつなぎ直す
static void spotifyCleanup() {
  if (relayOpen && spWs) {
    static const uint8_t CLOSE[6] = {0x88, 0x80, 0, 0, 0, 0};  // close フレーム (中身なし、マスク 0)
    relay->write(CLOSE, sizeof(CLOSE));
  }
  relayClose("spotify off");
  for (size_t i = 0; i < spRingSize / SP_SEG; i++) free(spSegs[i]);
  spRingSize = spRingHead = spRingLen = 0;
  spLists.clear();
  spLists.shrink_to_fit();
  spForgetTracks();
  playing = false;
  relayFailures = 0;
  relayRetryMs = 0;
}

// /spotify の WebSocket につながった。中継はすぐにプレイリストの一覧を送ってくる
static void spotifyConnected() {
  if (!spotifyAllocRing()) {
    relayClose("no memory");
    relayFailed();
    return;
  }
  spWs = true;
  wsHeadLen = 0;
  wsInFrame = false;
  spRead = spReported = 0;
  spCmds = 0;  // 中継は接続ごとに数え直す
  spAskedPage[0] = spAskedPage[1] = -1;  // 頼んでいた曲名の返事は来ない
  spLastReport = millis();
  spRingHead = spRingLen = 0;
  spWaitZ = false;
  spReset = true;
  spBuffering = true;
  spNote = "";
  relayStatus = "connected";
  // 切れる前に鳴らしていたら、続きを頼む (中継は切れたときに go-librespot を止めている)
  if (spActive && !spPaused) spSend("R");
  Serial.printf("spotify: connected (win %d, ring %u)\n", SP_WIN, (unsigned)spRingSize);
  spDraw();
}

// 音以外のフレームを1つ処理する
static void onSpFrame(char kind, const String &text) {
  int a = text.indexOf('\t');
  int b = a < 0 ? -1 : text.indexOf('\t', a + 1);
  switch (kind) {
    case 'Z':
      spWaitZ = false;
      break;
    case 'L': {  // 番号 TAB 総数 TAB 名前。0 番から順に全部来る
      if (b < 0) break;
      int i = text.substring(0, a).toInt();
      int total = text.substring(a + 1, b).toInt();
      if (i == 0) spLists.clear();
      if (spLists.size() < SP_LISTS_MAX) spLists.push_back(text.substring(b + 1));
      if (i + 1 >= total || spLists.size() >= SP_LISTS_MAX) {
        if (spSel >= (int)spLists.size()) spSel = 0;
        Serial.printf("spotify: %u playlists\n", (unsigned)spLists.size());
        spDraw();  // 全部届いてから1回だけ描く
      }
      break;
    }
    case 'T':  // 曲名 TAB アーティスト
      spTitle = a < 0 ? text : text.substring(0, a);
      spArtist = a < 0 ? String() : text.substring(a + 1);
      marqReset(spNowMarq);  // 新しい曲名は頭から流す
      lastSpScroll = millis();
      spNote = "";
      Serial.println("spotify: new track");
      spDraw();
      break;
    case 'S': {  // 状態 TAB シャッフル TAB プレイリストの番号 TAB 中継が受け取った操作の数 TAB 曲の番号
      if (b < 0) break;
      int c = text.indexOf('\t', b + 1);
      int d = c < 0 ? -1 : text.indexOf('\t', c + 1);
      spState = text.substring(0, a);
      spShuffle = text.substring(a + 1, b).toInt() != 0;
      spPlaying = text.substring(b + 1, c < 0 ? text.length() : c).toInt();
      spPlayingTrack = d < 0 ? -1 : text.substring(d + 1).toInt();
      // 音の後ろで待っていた古い知らせ (こちらが送った操作をまだ受け取っていない中継が送ったもの) では、
      // 一時停止を上書きしない。新しい知らせ (スマホの Spotify で止めたときなど) には合わせる
      if (c >= 0 && (uint32_t)text.substring(c + 1).toInt() >= spCmds) {
        if (spState == "paused") spPaused = true;
        if (spState == "playing") spPaused = false;
        if (spState == "playing") spActive = true;
        if (spState == "stopped") spActive = false;
      }
      spNote = "";
      Serial.printf("spotify: %s shuffle=%d list=%d track=%d\n", spState.c_str(), spShuffle, spPlaying, spPlayingTrack);
      spDraw();
      break;
    }
    case 'I': {  // プレイリストの番号 TAB 曲の番号 TAB 曲の数 TAB 曲名 TAB アーティスト (Q の返事)
      int c = b < 0 ? -1 : text.indexOf('\t', b + 1);
      if (c < 0 || text.substring(0, a).toInt() != spTrackList) break;  // 別のプレイリストを開いた
      int d = text.indexOf('\t', c + 1);
      int i = text.substring(a + 1, b).toInt();
      spTrackTotal = text.substring(b + 1, c).toInt();
      if (i >= 0) {
        spTrackNames[i % SP_NAMES] = text.substring(c + 1, d < 0 ? text.length() : d);
        spTrackArtists[i % SP_NAMES] = d < 0 ? String() : text.substring(d + 1);
        spTrackNameAt[i % SP_NAMES] = i;
      }
      // 描き直すのは選んでいる曲の名前が届いたとき (と空のプレイリスト) だけ。1回に 16 曲届く
      if (spView == SP_TRACKS && (i == spTrackSel || i < 0)) {
        spResetTrackScroll();
        spDraw();
      }
      break;
    }
    case 'E':
      spNote = text;
      Serial.printf("spotify: error \"%s\"\n", text.c_str());
      spDraw();
      break;
  }
}

// 届いたバイト列を WebSocket のフレームに解く。中継から来るフレームはマスクされていない
static void wsFeed(const uint8_t *p, size_t n) {
  while (n) {
    if (!wsInFrame) {
      wsHead[wsHeadLen++] = *p++;
      n--;
      if (wsHeadLen < 2) continue;
      uint8_t len7 = wsHead[1] & 0x7F;
      size_t need = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0);
      if (wsHeadLen < need) continue;
      wsOp = wsHead[0] & 0x0F;
      wsLeft = len7;
      if (len7 == 126) wsLeft = (wsHead[2] << 8) | wsHead[3];
      if (len7 == 127) wsLeft = (wsHead[6] << 24) | (wsHead[7] << 16) | (wsHead[8] << 8) | wsHead[9];
      wsHeadLen = 0;
      if (wsLeft == 0) {  // 中身の無いフレーム (種類のバイトも無い)
        if (wsOp == 0x8) {
          relayClose("spotify closed");
          return;
        }
        continue;
      }
      wsInFrame = true;
      wsKind = 0;
      wsText = "";
    } else if (wsOp == 0x2 && wsKind == 0) {
      wsKind = (char)*p++;
      n--;
      wsLeft--;
    } else {
      size_t m = std::min((size_t)wsLeft, n);
      if (wsOp == 0x2 && wsKind == 'A') {
        if (!spWaitZ) ringPut(p, m);  // Z を待っている間は前の曲なので捨てる
      } else {
        for (size_t i = 0; i < m && wsText.length() < SP_TEXT_MAX; i++) wsText += (char)p[i];
      }
      p += m;
      n -= m;
      wsLeft -= m;
    }
    if (wsInFrame && wsLeft == 0 && (wsOp != 0x2 || wsKind)) {
      wsInFrame = false;
      if (wsOp == 0x8) {
        relayClose("spotify closed");
        return;
      }
      if (wsOp == 0x2 && wsKind != 'A') onSpFrame(wsKind, wsText);
    }
  }
}

// 中継から届いた分を、リングの空きの範囲で読む。空きが無ければ読まないので、中継はそこで止まる
// (TCP の受信窓が閉じ、中継が送れるのも win まで)。そのときは状態の知らせも音の後ろで待つ
static void pollSpotify() {
  if (!relayOpen || !relay) return;
  uint8_t buf[256];
  bool full = false;
  int n = 0;
  for (size_t total = 0; total < SP_POLL_MAX; total += n) {
    size_t want = spWaitZ ? sizeof(buf) : std::min(sizeof(buf), ringFree());
    // 一時停止中は、リングが一杯でも音以外のフレームは1バイトずつ読み進める。中継は一時停止中は音を送らないので、
    // スマホの Spotify で再開されたのに気づける (音のフレームに当たったら、そこで止める)
    if (!want && spPaused && !(wsInFrame && wsKind == 'A')) want = 1;
    if (want == 0) {
      full = true;
      break;
    }
    n = relay->read(buf, want);
    if (n <= 0) break;
    spRead += n;
    lastRelayData = millis();
    wsFeed(buf, n);
    if (!relayOpen) return;  // close フレームが来た
  }
  if (full) lastRelayData = millis();  // 届かないのではなく読んでいないだけ (一時停止中など)
  if (spRead - spReported >= SP_CREDIT_STEP || millis() - spLastReport >= SP_KEEPALIVE_MS) spReportRead();
  if (n < 0 || !relay->connected()) {
    relayClose("relay lost");
  } else if (millis() - lastRelayData >= RELAY_IDLE_MS) {
    relayClose("relay silent");
  }
}

// 鳴らせる MP3 の量。リングと、デコーダの入力に移してまだデコードしていない分 (mp3Refill() は一度に 4KB まで移す)
static size_t spQueued(const Mp3Decoder *m) {
  size_t inDecoder = m->stream.buffer ? (size_t)(m->stream.bufend - m->stream.next_frame) : 0;
  return spRingLen + inDecoder;
}

// Spotify モードで中継とつながっている間、ここで回る。スイッチと中継を見ながら、受信リングの MP3 をデコードして鳴らす。
// 一時停止中や、溜まるのを待つ間は無音を流す (feedSilence() の説明を参照)。モードを抜けるか、接続が切れたら返す
static void playSpotify() {
  // 省電力にしないと Bluetooth と無線を取り合って途切れる (SD と同じ)。"wifi fast" で常時受信にもできる
  if (wifiFast) WiFi.noLowPowerMode();
  else WiFi.defaultLowPowerMode();

  Mp3Decoder *m = &mp3;
  mp3Start(m, nullptr);
  spReset = false;
  uint32_t rateMul = 1;
  uint32_t frames = 0;
  uint32_t underflows = 0;
  uint32_t starved = 0;     // リングが空になって、溜まるのを待った回数
  unsigned long started = millis();
  unsigned long lastCheck = 0;
  playing = false;
  a2dp.getUnderflow();

  while (spMode && relayOpen && spWs) {
    if (WiFi.status() != WL_CONNECTED) {
      relayClose("no wifi");
      break;
    }
    pollButton();
    pollSpotify();
    handleCommand(nextCommand());
    if (!spMode || !relayOpen || !spWs) break;

    if (spReset) {
      mp3End(m);
      mp3Start(m, nullptr);
      spReset = false;
    }

    // リングが半分たまったら鳴らし始める
    bool want = !spPaused && !spWaitZ && a2dp.connected();
    if (want && spBuffering && spQueued(m) >= spRingSize / 2) {
      spBuffering = false;
      startBars();
      spDraw();
    }
    if (!want || spBuffering) {
      if (playing) {
        playing = false;  // 2行目を棒から文字に戻す
        spDraw();
      }
      feedSilence();
      delay(1);
      continue;
    }
    if (spQueued(m) < SP_DECODE_MIN) {
      // 届くのが追いつかなかった。溜まるまで無音で待つ
      spBuffering = true;
      starved++;
      Serial.printf("spotify: buffering at %lus\n", (millis() - started) / 1000);
      continue;
    }

    // MP3 の1フレームを書く空きと、pcmChunk の端数を書き出す空きがあるまで待つ (playSd() と同じ)
    if ((size_t)a2dp.availableForWrite() < 1152 * 4 * rateMul + sizeof(pcmChunk)) {
      delay(1);
      continue;
    }
    if (!mp3Decode(m)) {
      // リングが空になっただけなら次に届いた分で続ける。それ以外のエラーならやり直す
      if (m->stream.error != MAD_ERROR_BUFLEN) spReset = true;
      continue;
    }
    frames++;

    struct mad_pcm &pcm = m->synth.pcm;
    rateMul = pcm.samplerate ? (uint32_t)A2DP_RATE / pcm.samplerate : 0;
    if (rateMul == 0 || rateMul > 4 || rateMul * pcm.samplerate != (uint32_t)A2DP_RATE) {
      Serial.printf("spotify: unsupported %uHz\n", pcm.samplerate);
      rateMul = 1;
      continue;
    }
    if (pcm.channels == 1) {
      for (size_t i = 0; i < pcm.length; i++) pcm.samplesX[i][1] = pcm.samplesX[i][0];
    }
    writePcm(&pcm.samplesX[0][0], pcm.length, 2, rateMul);

    if (millis() - lastBarDraw >= BAR_MS) drawBars();
    if (millis() - lastCheck >= 500) {
      lastCheck = millis();
      if (a2dp.getUnderflow() && millis() - started >= 1000) {
        underflows++;
        Serial.printf("underflow at %lus (ring %u/%u)\n", (millis() - started) / 1000,
                      (unsigned)spRingLen, (unsigned)spRingSize);
      }
    }
  }

  mp3End(m);
  playing = false;
  Serial.printf("spotify: %lu frames, buffering %lu times, underflow %lu times\n",
                (unsigned long)frames, (unsigned long)starved, (unsigned long)underflows);
  spDraw();
}

// "spotify" でモードに入る (入っていれば一覧と様子を出す)。"spotify 3" で 3 番目を鳴らす。"spotify off" で抜ける
static void spotifySerial(const String &cmd) {
  String arg = cmd.substring(7);
  arg.trim();
  if (arg == "off") {
    spotifyOff();
    return;
  }
  if (!spMode) {
    spotifyOn();
    if (spMode && arg.length()) Serial.println("spotify: 一覧が届いてから番号を打つ");
    return;
  }
  if (arg.length()) {
    spPlay(arg.toInt() - 1);
    return;
  }
  // 名前は半角カナのバイト列なので、シリアルでは化ける
  for (size_t i = 0; i < spLists.size(); i++) {
    Serial.printf("%c%3u %s\n", (int)i == spPlaying ? '>' : ' ', (unsigned)(i + 1), spLists[i].c_str());
  }
  Serial.printf("spotify: ws=%d state=%s paused=%d wait=%d buffering=%d ring=%u/%u win=%d read=%lu note=\"%s\"\n",
                spWs, spState.c_str(), spPaused, spWaitZ, spBuffering, (unsigned)spRingLen, (unsigned)spRingSize,
                SP_WIN, (unsigned long)spRead, spNote.c_str());
}

// Spotify モードの間のコマンド。スイッチで出す pause / next / prev / shuffle は Spotify の操作にする
static void spotifyCommand(const String &cmd) {
  if (cmd == "pause") spTogglePause();
  else if (cmd == "next") spSkip(true);
  else if (cmd == "prev") spSkip(false);
  else if (cmd == "shuffle") spSend("F");
  else if (cmd.startsWith("vol")) setVolume(cmd);
  else if (cmd.startsWith("wifi")) setWiFi(cmd);
  else if (cmd.startsWith("lcd")) lcdCommand(cmd);
  else if (cmd == "status") printStatus();
  else Serial.println("Spotify モード中に使えるのは spotify [番号|off] / pause / next / prev / shuffle / vol / wifi / lcd / status");
}

// ---------------- コマンド ----------------

static void startAutoPlay(Step step) {
  nextStep = step;
  autoPlay = true;
  paused = false;
  playErrors = 0;
}

static void printSdTracks() {
  if (sdStale) setupSD();
  if (sdTracks.empty()) {
    Serial.println(sdReady ? "SD: no mp3" : "SD: no card");
    return;
  }
  for (size_t i = 0; i < sdTracks.size(); i++) {
    Serial.printf("%c%3u %s\n", (int)i == sdIndex ? '>' : ' ', (unsigned)(i + 1), sdTracks[i].c_str());
  }
}

static const char *HELP =
    "commands: sd / pause / next / prev / rand / stop / list / now / shuffle / vol [0-100|+|-] /"
    " wifi on|off|fast|save / bt / scan / relay / spotify [n|off] / status / lcd [chars|resync on|off]";

static void printStatus() {
  Serial.printf("wifi=%d ip=%s bt=%d auto=%d pause=%d shuffle=%d vol=%d sd=%d tracks=%u heap=%luKB page=%s button=%s text=\"%s\" relay=%d (%s) msgs=%u spotify=%d\n",
                WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str(),
                a2dp.connected(), autoPlay, paused, shuffle, volume, sdReady,
                (unsigned)sdTracks.size(), (unsigned long)(rp2040.getFreeHeap() / 1024), PAGE_NAMES[page],
                digitalRead(PIN_BUTTON) == LOW ? "pressed" : "released",
                morseText.c_str(), relayOpen, relayStatus.c_str(), (unsigned)msgCount, spMode);
}

static void handleCommand(String cmd) {
  if (!cmd.length()) return;
  if (cmd.startsWith("spotify")) {
    spotifySerial(cmd);
    return;
  }
  if (spMode) {
    spotifyCommand(cmd);
    return;
  }

  if (cmd == "sd") {
    // カードを挿し直したときのために、毎回読み直す
    autoPlay = false;
    if (setupSD()) {
      startAutoPlay(STEP_NEXT);
    } else {
      status("sd", sdReady ? "no mp3" : "no card");
    }
  } else if (cmd == "pause") {
    // 止まっているときは再生を始める (BOOTSEL と同じ)。曲の合間やイヤホンのつなぎ直しを
    // 待っている間なら一時停止を切り替え、次の曲を止めた状態で始めるかを決める
    if (autoPlay) togglePause();
    else startAutoPlay(STEP_NEXT);
  } else if (cmd == "next") {
    startAutoPlay(STEP_NEXT);
  } else if (cmd == "prev") {
    startAutoPlay(STEP_PREV);
  } else if (cmd == "rand") {
    startAutoPlay(STEP_RAND);
  } else if (cmd == "stop") {
    autoPlay = false;
    paused = false;
    status("espoke", "stopped");
  } else if (cmd == "list") {
    printSdTracks();
  } else if (cmd == "now") {
    // Spotify モードで一覧を手放した後は、sdIndex が一覧の外を指していることがある
    if (sdIndex >= 0 && sdIndex < (int)sdTracks.size()) {
      Serial.printf("%d/%u %s\n", sdIndex + 1, (unsigned)sdTracks.size(), sdTracks[sdIndex].c_str());
    } else {
      Serial.println("SD: not started");
    }
  } else if (cmd == "shuffle") {
    toggleShuffle();
  } else if (cmd.startsWith("wifi")) {
    setWiFi(cmd);
  } else if (cmd.startsWith("vol")) {
    setVolume(cmd);
  } else if (cmd.startsWith("lcd")) {
    lcdCommand(cmd);
  } else if (cmd == "scan") {
    autoPlay = false;
    a2dp.disconnect();
    a2dp.clearPairing();
    findAndConnect();
  } else if (cmd == "bt") {
    btConnecting = !a2dp.connected();  // System の Bluetooth で SW4 を押したのと同じ
  } else if (cmd == "relay") {
    // 中継につなぎ直す。失敗が続いて延びていた間隔も戻し、次の loop() ですぐつなぎにいく
    relayClose("reconnect");
    relayFailures = 0;
    relayRetryMs = 0;
  } else if (cmd == "status") {
    printStatus();
  } else {
    Serial.println(HELP);
  }
}

// ---------------- setup / loop ----------------

// Wi-Fi につなぎ始める。WiFi.begin() はつながるまで最大 30 秒待つので、待たない方を使う。
// つながったかは loop() が見る
static void startWiFi() {
  lastWiFiTry = millis();
  status("WiFi", WIFI_SSID);
  WiFi.beginNoBlock(WIFI_SSID, WIFI_PASS);
}

void setup() {
  // シリアルがつながるのは待たない (電源を入れたらすぐホームを出すため)。PC でログを見るときは、
  // つながる前の分は出ない
  Serial.begin(115200);

  setenv("TZ", CLOCK_TZ, 1);  // localtime_r() を日本時間にする。time() (UTC) と証明書の期限の確認は変わらない
  // TCP の Nagle を切る (TCP を使うのは中継だけ)。Spotify では読んだ量を 1KB ごとに小さく知らせるが、Nagle だと
  // 前の分の ACK が返るまで次を出さないので、知らせが遅れて中継が送るのを待つことになる
  WiFiClient::setDefaultNoDelay(true);
  tzset();
  applyVolume(VOLUME_DEFAULT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  for (KeyState &k : keys) pinMode(k.pin, INPUT_PULLUP);
  setupLCD();  // LiquidCrystal_I2C の init() が 1 秒待つので、ホームが出るのは電源を入れて 1 秒ほど後
  setupBars();
  // 電源が入ったらすぐホームを出す。Bluetooth・SD・Wi-Fi の準備はこのあと。様子は Music の画面に出る
  openPage(PAGE_HOME);

  if (!RELAY_HOST[0]) relayStatus = "no RELAY_HOST";

  a2dp.setName(BT_LOCAL_NAME);
  a2dp.setFrequency(A2DP_RATE);
  a2dp.setBufferSize(A2DP_BUFFER);
  a2dp.onConnect(onConnect);
  a2dp.onVolume(onVolume);
  // A2DP のバッファは大きいので、ヒープが細切れになる前 (SD の曲一覧を作る前) に確保させる
  if (!a2dp.begin()) {
    status("BT init failed", "out of memory?");
  }

  // BTstack は User Confirmation Request をアプリに投げるだけで、既定では自動応答
  // しない (hci.c の ssp_auto_accept は 0)。イヤホンも Pico も入力装置を持たない
  // NoInputNoOutput なので Just Works ペアリングになり、ここを自動承認しないと
  // SSP が確認待ちのまま止まる
  {
    BluetoothLock b;
    gap_ssp_set_auto_accept(true);
  }

  setupSD();
  startWiFi();
  Serial.println(HELP);
}

void loop() {
  // Wi-Fi は最初の1回でつながらないことがあるので、つながらなければ WIFI_RETRY_MS ごとにつなぎ直す。
  // 連続再生している間はやらない (外で聞いているときに、無線が電波を探して音が途切れないように)
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    status("WiFi ok", WiFi.localIP().toString());
    // ホームの時計を合わせ始める (待たない)。中継を使うときは relayConnect() が合わせるので任せる
    if (!RELAY_HOST[0] && time(nullptr) < CLOCK_SET_AFTER) NTP.begin("ntp.nict.jp", "pool.ntp.org");
  }
  wifiUp = up;
  if (wifiEnabled && !autoPlay && !up && millis() - lastWiFiTry >= WIFI_RETRY_MS) startWiFi();

  // Spotify モードを抜けた。/spotify との接続を切り、受信リングを返す (Discord には下でつなぎ直す)
  if (!spMode && (spRingSize || spWs)) spotifyCleanup();

  // イヤホンには頼まれたときだけつなぎにいく (btConnecting の説明を参照)。失敗の理由は status() が
  // Music の1行目に出したもの ("BT timeout" など) をそのまま System にも出す
  if (btConnecting) {
    btNote = findAndConnect() ? String() : mainLines[0];
    btConnecting = false;
    if (page == PAGE_SYSTEM) drawPage();
  }

  // Discord の中継につなぐ (Spotify モードでは /spotify に)。TLS の握手で 4〜8 秒止まるので、再生ループの外
  // (止まっている間か曲の合間) でだけ。止まる前にバッファを無音で埋めておく。待つ間隔 (relayRetryMs) は
  // 前回の結果で決まる (起動直後は 0)。送る文があれば、つながっていても待たずにつなぎ直す
  if (RELAY_HOST[0] && WiFi.status() == WL_CONNECTED &&
      (relayOutbox.length() || (!relayOpen && millis() - lastRelayTry >= relayRetryMs))) {
    fillSilence();
    relayConnect();
  }
  pollRelay();

  pollButton();
  handleCommand(nextCommand());

  // Spotify モードでは、つながっている間は playSpotify() の中で回る。つなぎ直すのを待つ間は無音を流す。
  // BOOTSEL は効かない
  if (spMode) {
    if (relayOpen && spWs) playSpotify();
    else feedSilence();
    return;
  }

  // 止まっているときの BOOTSEL は連続再生の開始。再生中は pollAbort() が
  // 次の曲へ送るので、ここでは autoPlay を見て二重に反応しないようにする
  if (!autoPlay && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      startAutoPlay(STEP_NEXT);
    }
  }

  // 止まっている間は無音を流しておく。連続再生中は曲の切れ目に無音を挟まないよう、ここでは書かない
  if (!autoPlay) {
    feedSilence();
    return;
  }

  // イヤホンが切れている間は待つ。イヤホンには自分からはつながないので、つなぎ方を出しておく
  // (同じ表示なら書き直さない)
  if (!a2dp.connected()) {
    if (mainLines[0] != "no earphone") status("no earphone", "System > BT");
    feedSilence();
    delay(100);
    return;
  }

  if (sdStale) setupSD();  // Spotify モードで手放した曲の一覧を読み直す
  if (sdTracks.empty()) {
    autoPlay = false;
    status("sd", sdReady ? "no mp3" : "no card");
    return;
  }
  sdIndex = pickSdTrack();
  PlayResult r = playSd(sdIndex);

  if (r == PLAY_STOP) {
    autoPlay = false;
    nextStep = STEP_NEXT;
  } else if (r == PLAY_ERROR) {
    // 同じ向きに次の曲へ進むので、読めない曲が1曲あっても飛ばして続く。続けて失敗したらカードを疑って止める
    if (++playErrors >= 3) {
      autoPlay = false;
      playErrors = 0;
      status("music stopped", "sd card?");
    }
  } else {
    playErrors = 0;
    if (r == PLAY_DONE) nextStep = STEP_NEXT;
    // PLAY_SKIP のときは pollAbort() が nextStep を設定済み
  }
}
