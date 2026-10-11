#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>

// ===========================================================================
//  設定はここだけ（センサの型・ピン・読み周期）
// ===========================================================================
#define SENSOR_BME280 1
#define SENSOR_AM2320 2

// センサの型: ここを書き換えるか、platformio.ini の build_flags で
//   -D SENSOR_TYPE=SENSOR_AM2320
// と与える。
#ifndef SENSOR_TYPE
#define SENSOR_TYPE SENSOR_BME280
#endif

// 表示器（TM1637）を使うか。押入れ等、誰も見ない場所に置くなら 0。
// 0 にすると TM1637 は一切叩かず、CLK/DIO の 2 本と発熱がまるごと減る。
// platformio.ini の build_flags で -D USE_DISPLAY=0 と与えてもよい。
#ifndef USE_DISPLAY
#define USE_DISPLAY 1
#endif

// ピン配置（board の型が変われば、直すのはここだけ）
#if USE_DISPLAY
#define CLK 5
#define DIO 2
#endif
#define I2C_SDA 8
#define I2C_SCL 9

// BME280 の I2C アドレス（0x77 で応える breakout も在る）
#define BME280_ADDR 0x76

// センサを読む周期。AM2320 の下限は 2.0 秒なので余裕を持たせる。
#define SENSOR_INTERVAL_MS 3000

// I2C クロック。AM2320 は公称 100kHz までだが、実機（10cm のジャンパ、外付け 4.7k
// プルアップ）で測ると 100kHz は 10 回中 10 回タイムアウト、80kHz 以下は CRC まで
// 含めて 10/10 成功、という崖になっていた。境界から離して 50kHz を採る。
// Arduino の既定は 100kHz なので、これを呼ばないと AM2320 は一切読めない。
#if SENSOR_TYPE == SENSOR_AM2320
#define I2C_CLOCK_HZ 50000
#else
#define I2C_CLOCK_HZ 100000
#endif
// ===========================================================================

#if SENSOR_TYPE == SENSOR_BME280
  // ドライバは下に自前で持つ（Adafruit_BME280 は I2C の失敗を捨てるので使わない）
  #define SENSOR_NAME "BME280"
  #define SENSOR_HAS_PRESSURE 1
#elif SENSOR_TYPE == SENSOR_AM2320
  #include <AM232X.h>
  #define SENSOR_NAME "AM2320"
  #define SENSOR_HAS_PRESSURE 0
  AM232X sensor;
#else
  #error "SENSOR_TYPE must be SENSOR_BME280 or SENSOR_AM2320"
#endif

#if USE_DISPLAY
  #include <TM1637Display.h>
  TM1637Display display(CLK, DIO);
#endif
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

#if USE_DISPLAY
const uint8_t SEG_T = 0b01111000;
const uint8_t SEG_H = 0b01110100;
const uint8_t SEG_AP[] = {0b01110111, 0b01110011, 0b00000000, 0b00000000};
#endif

float currentTemp = 0;
float currentHumidity = 0;
#if SENSOR_HAS_PRESSURE
float currentPressure = 0;
#endif
String scannedSSIDs = "";
// 機体 ID。WiFi STA の MAC をそのまま使う。USB の /dev/serial/by-id に出る名前と
// 同じ値になるので、有線で挿しても無線で叩いても同じ識別子で個体を指せる。
// DHCP でアドレスが変わっても追えるように、IP とは別に持つ。
String deviceId = "";

// センサの健全性。測定値とは別に持つ。2026-09-27、排気側のセンサを塩水で濡らした時に
// ファームは 0.0℃/0.0%RH を4時間ぶん「測定値」として配り続けた。読めていないことは
// 値の形（0 かどうか）から推測させるのではなく、独立したフラグで出す。
bool sensorOk = false;            // 直近の読み取りが成功したか
bool sensorEverRead = false;      // ブート後に一度でも成功したか
uint32_t sensorReadErrors = 0;    // 失敗の累計。間欠的な化けはここに出る
int sensorLastRv = 0;             // 直近の読み取り戻り値。応答なしと化けを区別する（BME280 は BME_ERR_*）
unsigned long lastGoodReadMs = 0; // 直近で成功した時刻

// 直近の成功読みからの経過秒。一度も成功していなければブートからの経過を返す
// （その場合 currentTemp 等は初期値のままで、測定値としての意味を持たない）。
uint32_t sensorAgeSeconds() {
    unsigned long since = sensorEverRead ? (millis() - lastGoodReadMs) : millis();
    return (uint32_t)(since / 1000UL);
}

// 起動時に I2C バスが掴まれていたか。ハングは電源を切るまで直らない故障に見えるので、
// 遠くから原因を切り分けられるよう外に出す。
bool i2cWasHung = false;

// I2C バスのハング解除。ESP32 が転送の途中でリセットされると、スレーブは自分の番の
// ビットを出し切るまで SDA を low に握ったまま残る。マスタ側は以後すべての読みに失敗し、
// センサが壊れたように見える。SCL を 9 回叩いて残りを吐かせ、STOP を作って解放する。
// 2026-09-29 にこれを疑って入れたが、その時の不応答はセンサが物理的に外されていた
// だけだった（`i2c_hung_at_boot` は false を返した）。ハングを実際に観測したわけではない。
// それでも残す価値はある: 起動時に数十マイクロ秒で済み、掴まれていたかを外に報告するので、
// 次に同じ症状が出た時にこの仮説を触らずに潰せる。
bool i2cBusRecover() {
    pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SCL, HIGH);
    pinMode(I2C_SDA, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(I2C_SDA) != LOW) {   // 掴まれていない。何もしない
        pinMode(I2C_SCL, INPUT);
        return false;
    }
    // 残りのビットを吐かせる。9 回でバイト境界に必ず戻る。
    for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
        digitalWrite(I2C_SCL, LOW);
        delayMicroseconds(10);
        digitalWrite(I2C_SCL, HIGH);
        delayMicroseconds(10);
    }
    // STOP を作る。SCL を high に保ったまま SDA を low→high。
    pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SDA, HIGH);
    delayMicroseconds(10);
    pinMode(I2C_SDA, INPUT);
    pinMode(I2C_SCL, INPUT);
    return true;
}

#if SENSOR_TYPE == SENSOR_BME280
// ---------------------------------------------------------------------------
//  BME280 を自前で読む
// ---------------------------------------------------------------------------
// Adafruit_BME280 (v2.3.0) を外した理由は2つある。
//  1. I2C の失敗を上に返さない。read24()/read16() は write_then_read() の戻り値を
//     捨てるので、失敗した転送は [レジスタ番地, スタックのゴミ…] のまま補正式に入り、
//     温度なら 180℃ 前後の「測定値」になる。NAN になるのは oversampling が SKIP の
//     時だけで、sensor_ok も read_errors も原理的に動かなかった。
//  2. 1 回の読みで温度レジスタを 3 回、別々の転送で読む（readHumidity() と
//     readPressure() が中で readTemperature() を呼ぶ）。2026-10-07 以降の
//     「温度だけ -147.0℃、同じ回の湿度・気圧は正常」はこの 1 回目だけが化けた形。
// ここでは 0xF7〜0xFE の 8 バイトを一括で読み（データシート推奨。3ch が同じ測定から
// 取れ、t_fine も 1 回で済む）、転送ごとに戻り値を見る。

// updateSensorData() が sensorLastRv に入れる理由コード。AM2320 の rv とは別体系。
#define BME_OK              0
#define BME_ERR_NOT_READY  -1   // 起動時に chip id / 校正係数が読めていない
#define BME_ERR_WRITE      -2   // 計測開始の書き込みが NACK
#define BME_ERR_TIMEOUT    -3   // status の measuring が下りない、または status が読めない
#define BME_ERR_READ       -4   // データの読み出しが失敗
#define BME_ERR_MISMATCH   -5   // 同じ 8 バイトを 2 回読んで一致しない（化け）
#define BME_ERR_SKIPPED    -6   // 生値がリセット値（0x80000 / 0x8000）= 測定されていない
#define BME_ERR_RAW        -7   // 生値が全 0 / 全 1。バスが張り付いた形
#define BME_ERR_RANGE      -8   // 補正後が仕様の測定範囲外、または湿度が補正式でクランプ

struct {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1; int16_t H2; uint8_t H3; int16_t H4, H5; int8_t H6;
} bmeCal;
bool bmeReady = false;

bool bmeWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(BME280_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// 読めた時だけ true。失敗した時の buf の中身は使わないこと。
bool bmeRead(uint8_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(BME280_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint16_t)BME280_ADDR, len) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

// 同じ範囲を 2 回読み、一致した時だけ true。データレジスタにも校正係数にも CRC が
// 無いので、これが化けを捕まえる唯一の手段になる。forced モードの sleep 中は
// レジスタが動かないので、正しく読めていれば必ず一致する。
bool bmeReadTwice(uint8_t reg, uint8_t *buf, size_t len, bool *mismatch) {
    uint8_t again[32];
    *mismatch = false;
    if (len > sizeof(again)) return false;
    if (!bmeRead(reg, buf, len) || !bmeRead(reg, again, len)) return false;
    if (memcmp(buf, again, len) != 0) { *mismatch = true; return false; }
    return true;
}

bool sensorBegin() {
    bmeReady = false;
    uint8_t id;
    if (!bmeRead(0xD0, &id, 1) || id != 0x60) return false;   // 0x60 = BME280

    // 電源投入直後は NVM から校正係数を写している最中（status bit0）。
    uint8_t st = 1;
    for (int i = 0; i < 10 && (st & 0x01); i++) {
        if (!bmeRead(0xF3, &st, 1)) return false;
        if (st & 0x01) delay(2);
    }

    uint8_t a[26], b[7];
    bool mm;
    if (!bmeReadTwice(0x88, a, sizeof(a), &mm)) return false;
    if (!bmeReadTwice(0xE1, b, sizeof(b), &mm)) return false;
    bmeCal.T1 = a[0]  | a[1]  << 8;  bmeCal.T2 = a[2]  | a[3]  << 8;  bmeCal.T3 = a[4]  | a[5]  << 8;
    bmeCal.P1 = a[6]  | a[7]  << 8;  bmeCal.P2 = a[8]  | a[9]  << 8;  bmeCal.P3 = a[10] | a[11] << 8;
    bmeCal.P4 = a[12] | a[13] << 8;  bmeCal.P5 = a[14] | a[15] << 8;  bmeCal.P6 = a[16] | a[17] << 8;
    bmeCal.P7 = a[18] | a[19] << 8;  bmeCal.P8 = a[20] | a[21] << 8;  bmeCal.P9 = a[22] | a[23] << 8;
    bmeCal.H1 = a[25];
    bmeCal.H2 = b[0] | b[1] << 8;
    bmeCal.H3 = b[2];
    bmeCal.H4 = (int16_t)((int8_t)b[3] * 16 | (b[4] & 0x0F));
    bmeCal.H5 = (int16_t)((int8_t)b[5] * 16 | (b[4] >> 4));
    bmeCal.H6 = (int8_t)b[6];
    // 全 0 / 全 1 の係数はバスの張り付きで、気圧の式は P1 で割る。
    if (bmeCal.T1 == 0 || bmeCal.T1 == 0xFFFF || bmeCal.P1 == 0 || bmeCal.P1 == 0xFFFF) return false;

    // Adafruit の既定は MODE_NORMAL / 全ch ×16 / standby 0.5ms で、デューティが
    // 99.6% になる。ダイが自分の熱で温まり、その温度で湿度を補正するため湿度が
    // 低めに出る。Bosch の weather monitoring 推奨（forced / ×1 / フィルタ無し）に
    // 合わせる。3 秒間隔で forced なら 1 回 ~10ms、デューティは 0.3% に落ちる。
    if (!bmeWrite(0xF5, 0x00)) return false;   // config: フィルタ無し
    if (!bmeWrite(0xF2, 0x01)) return false;   // ctrl_hum: 湿度 ×1
    if (!bmeWrite(0xF4, 0x24)) return false;   // ctrl_meas: 温度 ×1 / 気圧 ×1 / sleep
    bmeReady = true;
    return true;
}

// 以下 3 つはデータシート 4.2.3 / 8.2 の整数版補正式そのまま。
int32_t bmeTFine(int32_t adcT) {
    int32_t v1 = ((((adcT >> 3) - ((int32_t)bmeCal.T1 << 1))) * (int32_t)bmeCal.T2) >> 11;
    int32_t d  = (adcT >> 4) - (int32_t)bmeCal.T1;
    int32_t v2 = (((d * d) >> 12) * (int32_t)bmeCal.T3) >> 14;
    return v1 + v2;
}

// Pa。補正式が 0 除算になる時は 0 を返す（範囲検査で落ちる）。
float bmePressure(int32_t adcP, int32_t tFine) {
    int64_t v1 = (int64_t)tFine - 128000;
    int64_t v2 = v1 * v1 * (int64_t)bmeCal.P6;
    v2 = v2 + v1 * (int64_t)bmeCal.P5 * 131072;
    v2 = v2 + (int64_t)bmeCal.P4 * 34359738368LL;
    v1 = ((v1 * v1 * (int64_t)bmeCal.P3) >> 8) + v1 * (int64_t)bmeCal.P2 * 4096;
    v1 = ((((int64_t)1) << 47) + v1) * ((int64_t)bmeCal.P1) >> 33;
    if (v1 == 0) return 0;
    int64_t p = 1048576 - adcP;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((int64_t)bmeCal.P9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((int64_t)bmeCal.P8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (int64_t)bmeCal.P7 * 16;
    return (float)p / 256.0f;
}

// %RH。補正式が 0 / 100 にクランプした時は *clamped を立てる。
float bmeHumidity(int32_t adcH, int32_t tFine, bool *clamped) {
    int32_t v = tFine - (int32_t)76800;
    v = (((((adcH << 14) - ((int32_t)bmeCal.H4 * 1048576) - (((int32_t)bmeCal.H5) * v)) +
           ((int32_t)16384)) >> 15) *
         (((((((v * ((int32_t)bmeCal.H6)) >> 10) * (((v * ((int32_t)bmeCal.H3)) >> 11) +
               ((int32_t)32768))) >> 10) + ((int32_t)2097152)) * ((int32_t)bmeCal.H2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)bmeCal.H1)) >> 4);
    *clamped = (v <= 0 || v >= 419430400);
    v = v < 0 ? 0 : v;
    v = v > 419430400 ? 419430400 : v;
    return (float)((uint32_t)v >> 12) / 1024.0f;
}

// 1 回測って読む。成功なら BME_OK を返し、t/h/p に値を入れる。失敗時は何も書かない。
int bmeMeasure(float *t, float *h, float *p) {
    if (!bmeReady) return BME_ERR_NOT_READY;
    // ctrl_hum も毎回書く。ctrl_hum は ctrl_meas を書いた時に初めて効く上、チップが
    // ブラウンアウトでリセットされると 0（湿度を測らない）に戻る。ctrl_meas だけを
    // 書き直していた Adafruit の作りでは、湿度だけ 0x8000 のまま固まり得た。
    if (!bmeWrite(0xF2, 0x01) || !bmeWrite(0xF4, 0x25)) return BME_ERR_WRITE;   // 0x25 = forced
    // ×1 ×3ch の最大計測時間は 9.3ms。書いた直後に status を見ると measuring が
    // まだ立っていないことがあるので、先に待ってから見る。
    delay(10);
    uint8_t st = 0x08;
    for (int i = 0; i < 50 && (st & 0x08); i++) {
        if (!bmeRead(0xF3, &st, 1)) return BME_ERR_TIMEOUT;
        if (st & 0x08) delay(1);
    }
    if (st & 0x08) return BME_ERR_TIMEOUT;

    uint8_t d[8];
    bool mismatch;
    if (!bmeReadTwice(0xF7, d, sizeof(d), &mismatch)) return mismatch ? BME_ERR_MISMATCH : BME_ERR_READ;
    int32_t adcP = (int32_t)d[0] << 12 | (int32_t)d[1] << 4 | d[2] >> 4;
    int32_t adcT = (int32_t)d[3] << 12 | (int32_t)d[4] << 4 | d[5] >> 4;
    int32_t adcH = (int32_t)d[6] << 8  | d[7];
    // リセット値のまま = 測定されなかった。README の 20.3℃ / 80.6% / 758.6hPa はこれ。
    if (adcT == 0x80000 || adcP == 0x80000 || adcH == 0x8000) return BME_ERR_SKIPPED;
    // 全 0 / 全 1 は SDA が張り付いた形で、2 回読んでも同じに揃う。-147.0℃ は推測では
    // 温度の生値 0 から出ている。
    if (adcT == 0 || adcP == 0 || adcT == 0xFFFFF || adcP == 0xFFFFF || adcH == 0xFFFF)
        return BME_ERR_RAW;

    int32_t tFine = bmeTFine(adcT);
    float tc = (float)((tFine * 5 + 128) >> 8) / 100.0f;
    float pa = bmePressure(adcP, tFine);
    bool clamped;
    float rh = bmeHumidity(adcH, tFine, &clamped);
    // 範囲はデータシートの測定範囲（温度 -40〜85℃、気圧 300〜1100hPa）。湿度の 0.0 /
    // 100.0 は補正式のクランプで、2026-09 の化けで何度も出た値。屋内では本物の 100%
    // も来ないので、クランプに当たった読みは捨てる。
    if (tc < -40.0f || tc > 85.0f || pa < 30000.0f || pa > 110000.0f || clamped) return BME_ERR_RANGE;

    *t = tc;
    *h = rh;
    *p = pa / 100.0f;
    return BME_OK;
}
#else
bool sensorBegin() {
    return sensor.begin();
}
#endif

// センサを実際に叩くのはここだけ。呼び出すのは loop() だけで、
// HTTP の処理からは呼ばない（AM2320 の 2.0 秒規則を外から破られないため）。
void updateSensorData() {
    bool ok;
#if SENSOR_TYPE == SENSOR_BME280
    // 起動時に読めていなければ、ここで掴み直す（一時的な接触不良で一生読めないのを避ける）。
    if (!bmeReady) sensorBegin();
    float t, h, pr;
    int rv = bmeMeasure(&t, &h, &pr);
    sensorLastRv = rv;
    ok = (rv == BME_OK);
    if (ok) {
        currentTemp = t;
        currentHumidity = h;
        currentPressure = pr;
    } else {
        // 失敗した時は前回値を残す。何が起きたかは sensor_ok / read_errors / last_rv に出る。
        Serial.println("BME280 read error: " + String(rv));
    }
#else
    // 起こす作法と 2.0 秒の下限はライブラリ側が持っている。
    int rv = sensor.read();
    // 呼ぶのが早すぎただけ。新しい値は無いが故障でもないので、何も動かさない
    // （成功にも失敗にも数えない。でないと失敗数と経過秒の意味が濁る）。
    if (rv == AM232X_READ_TOO_FAST) return;
    sensorLastRv = rv;
    ok = (rv == AM232X_OK);
    if (ok) {
        currentTemp = sensor.getTemperature();
        currentHumidity = sensor.getHumidity();
    } else {
        Serial.println("AM2320 read error: " + String(rv));
    }
#endif
    sensorOk = ok;
    if (ok) {
        sensorEverRead = true;
        lastGoodReadMs = millis();
    } else if (sensorReadErrors < UINT32_MAX) {
        sensorReadErrors++;
    }
}

// 人が見る画面にも 0.0 を測定値として出さない。「測っていない」と「0℃」は別物。
String sensorValueText(float v, int digits, const char *suffix) {
    if (!sensorEverRead) return String("—");
    return String(v, digits) + suffix;
}

#if USE_DISPLAY
void updateDisplay(char type, float value) {
    uint8_t data[4];
    data[0] = (type == 't') ? SEG_T : SEG_H;
    if (WiFi.status() == WL_CONNECTED) data[0] |= 0b10000000;

    int val = (int)(value * 10);
    if (val < 0) val = 0;
    if (val > 999) val = 999;
    data[1] = display.encodeDigit((val / 100) % 10);
    data[2] = display.encodeDigit((val / 10) % 10) | 0b10000000;
    data[3] = display.encodeDigit(val % 10);
    display.setSegments(data);
}
#endif

void scanNetworks() {
    Serial.println("Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    scannedSSIDs = "";
    
    if (n > 0) {
        String added[30];
        int addedCount = 0;
        
        for (int i = 0; i < n && addedCount < 30; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;
            
            bool isDup = false;
            for (int j = 0; j < addedCount; j++) {
                if (added[j] == ssid) { isDup = true; break; }
            }
            if (isDup) continue;
            
            added[addedCount++] = ssid;
            int rssi = WiFi.RSSI(i);
            String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "" : "🔒";
            
            String signal;
            if (rssi > -50) signal = "▂▄▆█";
            else if (rssi > -60) signal = "▂▄▆_";
            else if (rssi > -70) signal = "▂▄__";
            else signal = "▂___";
            
            scannedSSIDs += "<option value='" + ssid + "'>" + enc + " " + ssid + " " + signal + "</option>";
        }
    }
    WiFi.scanDelete();
    Serial.println("Scan complete: " + String(n) + " networks");
}

String makeHTML() {
    String s = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<title>ESP32-C3 Sensor</title>";
    s += "<style>";
    s += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; margin:0; padding:15px; box-sizing:border-box;}";
    s += ".container{max-width:400px; margin:0 auto;}";
    s += ".card{background:white; border-radius:20px; padding:25px; box-shadow:0 10px 40px rgba(0,0,0,0.2); margin-bottom:20px;}";
    s += "h1{margin:0 0 20px 0; color:#333; font-size:1.5em;}";
    s += ".sensor-grid{display:grid; gap:15px;}";
    s += ".sensor-item{background:linear-gradient(135deg,#f5f7fa 0%,#c3cfe2 100%); border-radius:15px; padding:20px; text-align:center;}";
    s += ".sensor-value{font-size:2.5em; font-weight:bold; color:#333;}";
    s += ".sensor-label{font-size:0.9em; color:#666; margin-top:5px;}";
    s += ".temp .sensor-value{color:#e74c3c;}";
    s += ".humid .sensor-value{color:#3498db;}";
    s += ".press .sensor-value{color:#9b59b6;}";
    s += "h2{margin:0 0 15px 0; color:#333; font-size:1.2em;}";
    s += ".form-group{margin-bottom:15px;}";
    s += "select,input[type='text'],input[type='password']{width:100%; padding:15px; border:2px solid #e0e0e0; border-radius:10px; font-size:1em; box-sizing:border-box; transition:border-color 0.3s; background:white;}";
    s += "select{cursor:pointer;}";
    s += "select:focus,input[type='text']:focus,input[type='password']:focus{border-color:#667eea; outline:none;}";
    s += ".pass-container{position:relative;}";
    s += ".toggle-btn{position:absolute; right:15px; top:50%; transform:translateY(-50%); cursor:pointer; font-size:1.2em; user-select:none;}";
    s += ".btn{width:100%; padding:15px; border:none; border-radius:10px; font-size:1em; font-weight:bold; cursor:pointer; transition:transform 0.2s, box-shadow 0.2s;}";
    s += ".btn:active{transform:scale(0.98);}";
    s += ".btn-primary{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); color:white; box-shadow:0 4px 15px rgba(102,126,234,0.4);}";
    s += ".btn-secondary{background:linear-gradient(135deg,#a8a8a8 0%,#888 100%); color:white; box-shadow:0 4px 15px rgba(0,0,0,0.2); margin-bottom:10px;}";
    s += ".btn-danger{background:linear-gradient(135deg,#e74c3c 0%,#c0392b 100%); color:white; box-shadow:0 4px 15px rgba(231,76,60,0.4); margin-top:10px;}";
    s += ".status{background:#e8f5e9; border-radius:10px; padding:15px; margin-bottom:20px;}";
    s += ".status.disconnected{background:#ffebee;}";
    s += ".sensor-warn{background:#ffebee; color:#c0392b; border-radius:10px; padding:12px; margin-top:15px; font-size:0.85em; font-weight:bold;}";
    s += ".status-label{font-size:0.85em; color:#666;}";
    s += ".status-value{font-weight:bold; color:#333;}";
    s += ".manual-input{display:none; margin-top:10px;}";
    s += ".manual-input.show{display:block;}";
    s += "</style></head><body>";

    s += "<div class='container'>";
    s += "<div class='card'>";
    s += "<h1>🌡️ ESP32-C3 Sensor</h1>";
    s += "<div class='sensor-grid'>";
    s += "<div class='sensor-item temp'><div class='sensor-value'>" + sensorValueText(currentTemp, 1, "°") + "</div><div class='sensor-label'>温度 (℃)</div></div>";
    s += "<div class='sensor-item humid'><div class='sensor-value'>" + sensorValueText(currentHumidity, 1, "%") + "</div><div class='sensor-label'>湿度</div></div>";
#if SENSOR_HAS_PRESSURE
    s += "<div class='sensor-item press'><div class='sensor-value'>" + sensorValueText(currentPressure, 0, "") + "</div><div class='sensor-label'>気圧 (hPa)</div></div>";
#endif
    s += "</div>";
    if (!sensorOk) {
        s += "<div class='sensor-warn'>⚠ " SENSOR_NAME " を読めていません（失敗 " + String(sensorReadErrors);
        s += " 回 / 直近の成功から " + String(sensorAgeSeconds()) + " 秒";
        s += sensorEverRead ? "）</div>" : "・起動後まだ一度も成功していません）</div>";
    }
    s += "</div>";

    s += "<div class='card'>";
    if (WiFi.status() == WL_CONNECTED) {
        s += "<div class='status'>";
        s += "<div class='status-label'>接続中</div>";
        s += "<div class='status-value'>" + WiFi.SSID() + "</div>";
        s += "<div class='status-label'>IP: " + WiFi.localIP().toString() + "</div>";
        s += "<div class='status-label'>電波: " + String(WiFi.RSSI()) + " dBm</div>";
        s += "<div class='status-label'>ID: " + deviceId + "</div>";
        s += "</div>";
    } else {
        s += "<div class='status disconnected'>";
        s += "<div class='status-label'>APモード</div>";
        s += "<div class='status-value'>ESP32C3-Setup</div>";
        s += "<div class='status-label'>IP: 192.168.4.1</div>";
        s += "<div class='status-label'>ID: " + deviceId + "</div>";
        s += "</div>";
    }

    s += "<h2>📶 WiFi設定</h2>";
    s += "<form action='/save' method='get'>";
    s += "<div class='form-group'>";
    s += "<select name='s' id='ssidSelect' onchange='onSSIDChange()'>";
    s += "<option value=''>-- ネットワークを選択 --</option>";
    s += scannedSSIDs;
    s += "<option value='__manual__'>✏️ 手動で入力...</option>";
    s += "</select></div>";
    s += "<div class='form-group manual-input' id='manualInput'>";
    s += "<input type='text' id='manualSSID' placeholder='SSIDを入力'></div>";
    s += "<button type='button' class='btn btn-secondary' onclick='location.href=\"/scan\"'>🔄 再スキャン</button>";
    s += "<div class='form-group pass-container'><input type='password' name='p' id='pass' placeholder='パスワード'>";
    s += "<span class='toggle-btn' onclick='togglePass()'>👁</span></div>";
    s += "<button type='submit' class='btn btn-primary'>設定を保存</button>";
    s += "</form>";
    s += "<button class='btn btn-danger' onclick='if(confirm(\"WiFi設定をリセットしますか？\"))location.href=\"/reset\"'>設定をリセット</button>";
    s += "</div></div>";

    s += "<script>";
    s += "function togglePass(){var p=document.getElementById('pass');var b=document.querySelector('.toggle-btn');if(p.type==='password'){p.type='text';b.textContent='🔒';}else{p.type='password';b.textContent='👁';}}";
    s += "function onSSIDChange(){var sel=document.getElementById('ssidSelect');var manual=document.getElementById('manualInput');var manualField=document.getElementById('manualSSID');";
    s += "if(sel.value==='__manual__'){manual.classList.add('show');manualField.required=true;}else{manual.classList.remove('show');manualField.required=false;}}";
    s += "document.querySelector('form').onsubmit=function(){var sel=document.getElementById('ssidSelect');var manualField=document.getElementById('manualSSID');";
    s += "if(sel.value==='__manual__'&&manualField.value){sel.name='';var h=document.createElement('input');h.type='hidden';h.name='s';h.value=manualField.value;this.appendChild(h);}return true;};";
    s += "</script></body></html>";
    return s;
}

String makeResultHTML(String ssid) {
    String s = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<style>";
    s += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; margin:0; display:flex; align-items:center; justify-content:center; padding:15px; box-sizing:border-box;}";
    s += ".card{background:white; border-radius:20px; padding:40px; box-shadow:0 10px 40px rgba(0,0,0,0.2); text-align:center; max-width:350px;}";
    s += ".icon{font-size:4em; margin-bottom:20px;}h1{margin:0 0 10px 0; color:#333;}p{color:#666; margin:10px 0;}";
    s += ".ssid{font-weight:bold; color:#333;}.warning{color:#e74c3c; font-weight:bold; margin-top:20px;}";
    s += "</style></head><body><div class='card'><div class='icon'>✅</div><h1>設定完了</h1>";
    s += "<p>SSID: <span class='ssid'>" + ssid + "</span></p>";
    s += "<p class='warning'>⚡ 本体の電源を入れ直してください</p></div></body></html>";
    return s;
}

String makeScanHTML() {
    String s = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<style>";
    s += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; margin:0; display:flex; align-items:center; justify-content:center;}";
    s += ".card{background:white; border-radius:20px; padding:40px; box-shadow:0 10px 40px rgba(0,0,0,0.2); text-align:center;}";
    s += ".icon{font-size:4em; margin-bottom:20px; animation:spin 1s linear infinite;}";
    s += "@keyframes spin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}";
    s += "h1{margin:0 0 10px 0; color:#333;}p{color:#666;}";
    s += "</style></head><body><div class='card'><div class='icon'>📡</div><h1>スキャン中...</h1>";
    s += "<p>ネットワークを検索しています</p></div>";
    s += "<script>setTimeout(function(){location.href='/';},100);</script></body></html>";
    return s;
}

void startAP() {
    Serial.println("Starting AP Mode...");

    // 完全停止
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    // APモード開始
    WiFi.mode(WIFI_AP);
    delay(200);

    // IP設定
    IPAddress apIP(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, subnet);
    delay(100);

    // AP起動
    bool apStarted = WiFi.softAP("ESP32C3-Setup", "", 1, false, 4);
    delay(200);

    // ★ softAP後にTxPower設定
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    delay(100);
    
    Serial.print("TxPower after softAP: ");
    Serial.println(WiFi.getTxPower());

    // まだ34なら、さらに低い値を試す
    if (WiFi.getTxPower() > 30) {
        Serial.println("TxPower still high, trying lower...");
        WiFi.setTxPower(WIFI_POWER_5dBm);
        delay(100);
        Serial.print("TxPower retry: ");
        Serial.println(WiFi.getTxPower());
    }

    if (apStarted) {
        Serial.println("=== AP Started ===");
        Serial.print("SSID: ESP32C3-Setup  CH: ");
        Serial.println(WiFi.channel());
        Serial.print("IP: ");
        Serial.println(WiFi.softAPIP());
        Serial.print("Final TxPower: ");
        Serial.println(WiFi.getTxPower());
#if USE_DISPLAY
        display.setSegments(SEG_AP);
#endif
        scanNetworks();
    } else {
        Serial.println("!!! AP FAILED !!!");
        delay(3000);
        ESP.restart();
    }

    dnsServer.start(53, "*", apIP);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ESP32-C3 SuperMini Sensor ===");

#if USE_DISPLAY
    display.setBrightness(0x05);
    display.showNumberDec(8888);
#endif

    // Wire より先に。バスが掴まれた状態で begin しても解けない。
    i2cWasHung = i2cBusRecover();
    if (i2cWasHung) Serial.println("I2C bus was hung at boot; recovered");

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_CLOCK_HZ);
    if (!sensorBegin()) {
        Serial.println(SENSOR_NAME " not found!");
    } else {
        Serial.println(SENSOR_NAME " OK");
    }
    updateSensorData();

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    prefs.begin("wifi-store", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    Serial.print("Stored SSID: [");
    Serial.print(ssid);
    Serial.println("]");

    bool connected = false;

    if (ssid.length() > 0) {
        Serial.println("Connecting to WiFi...");
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.begin(ssid.c_str(), pass.c_str());

        int retry = 0;
        while (retry < 20) {
            delay(500);
            Serial.print(".");
            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                break;
            }
            retry++;
        }
        Serial.println();
    }

    if (connected) {
        Serial.println("WiFi Connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("Starting AP...");
        startAP();
    }

    deviceId = WiFi.macAddress();
    Serial.println("Device ID: " + deviceId);

    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", makeHTML());
    });

    server.on("/scan", HTTP_GET, []() {
        server.send(200, "text/html", makeScanHTML());
        scanNetworks();
    });

    server.on("/save", HTTP_GET, []() {
        String ssid = server.arg("s");
        String pass = server.arg("p");
        prefs.begin("wifi-store", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        server.send(200, "text/html", makeResultHTML(ssid));
        Serial.println("Config Saved: " + ssid);
    });

    server.on("/reset", HTTP_GET, []() {
        prefs.begin("wifi-store", false);
        prefs.clear();
        prefs.end();
        String s = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
        s += "<style>body{font-family:sans-serif; background:linear-gradient(135deg,#e74c3c 0%,#c0392b 100%); min-height:100vh; margin:0; display:flex; align-items:center; justify-content:center;}";
        s += ".card{background:white; border-radius:20px; padding:40px; text-align:center;}</style></head><body>";
        s += "<div class='card'><div style='font-size:4em;'>🔄</div><h1>リセット完了</h1><p>再起動します...</p></div></body></html>";
        server.send(200, "text/html", s);
        Serial.println("Settings cleared. Restarting...");
        delay(1000);
        ESP.restart();
    });

    // 直近値を返すだけ。ここでセンサを叩くと、外から周期的に叩かれた分だけ
    // 読みが増えて AM2320 の 2.0 秒規則を破る。
    server.on("/api/data", HTTP_GET, []() {
        bool up = (WiFi.status() == WL_CONNECTED);
        String json = "{\"id\":\"" + deviceId + "\"";
        // 一度も読めていないうちは temperature/humidity を出さない。初期値の 0.0 を
        // 測定値として配ると下流には区別がつかない。json_exporter はキーが無ければ
        // そのメトリクスを出さないだけでスクレイプは 200 で成功するので、消えた穴は
        // 下の sensor_ok が埋める（実測で確認済み）。rssi/pressure と同じ流儀。
        if (sensorEverRead) {
            json += ",\"temperature\":" + String(currentTemp, 1) + ",\"humidity\":" + String(currentHumidity, 1);
#if SENSOR_HAS_PRESSURE
            json += ",\"pressure\":" + String(currentPressure, 1);
#endif
        }
        // 健全性は値と独立に、必ず出す。値が消えても原因はここに残る。
        // json_exporter は true/false を 1/0 に変換する（実測で確認済み）。
        json += ",\"sensor\":\"" SENSOR_NAME "\"";
        json += ",\"sensor_ok\":" + String(sensorOk ? "true" : "false");
        json += ",\"read_errors\":" + String(sensorReadErrors);
        json += ",\"last_read_age_s\":" + String(sensorAgeSeconds());
        // 起動時にバスが掴まれていたか。読めない原因がバスかセンサかを遠くから分ける。
        json += ",\"i2c_hung_at_boot\":" + String(i2cWasHung ? "true" : "false");
        // 直近の戻り値。応答なし(接続失敗)と CRC 不一致は原因も対処も違う。
        json += ",\"last_rv\":" + String(sensorLastRv);
        json += ",\"wifi_connected\":" + String(up ? "true" : "false");
        // 電波強度は繋がっている時だけ。AP モードでは意味を持たないので、
        // pressure と同じ流儀でキーごと出さない（null は返さない）。
        if (up) json += ",\"rssi\":" + String(WiFi.RSSI());
        json += ",\"ip\":\"" + (up ? WiFi.localIP().toString() : "192.168.4.1") + "\"}";
        server.send(200, "application/json", json);
    });

    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });

    server.begin();
    Serial.println("HTTP Server Started");
}

void loop() {
    if (WiFi.getMode() == WIFI_AP) {
        dnsServer.processNextRequest();
    }
    server.handleClient();

    static unsigned long lastUpdate = 0;
#if USE_DISPLAY
    static bool showTemp = true;
#endif
    if (millis() - lastUpdate > SENSOR_INTERVAL_MS) {
        // AP モードでも読む（表示は AP のまま、web ページには直近値が要る）
        updateSensorData();
#if USE_DISPLAY
        if (WiFi.getMode() != WIFI_AP) {
            updateDisplay(showTemp ? 't' : 'h', showTemp ? currentTemp : currentHumidity);
            showTemp = !showTemp;
        }
#endif
        lastUpdate = millis();
    }
}
