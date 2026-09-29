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
  #include <Adafruit_Sensor.h>
  #include <Adafruit_BME280.h>
  #define SENSOR_NAME "BME280"
  #define SENSOR_HAS_PRESSURE 1
  Adafruit_BME280 sensor;
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
int sensorLastRv = 0;             // 直近の読み取り戻り値。応答なしと化けを区別する
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

bool sensorBegin() {
#if SENSOR_TYPE == SENSOR_BME280
    if (!sensor.begin(BME280_ADDR, &Wire)) return false;
    // Adafruit の既定は MODE_NORMAL / 全ch ×16 / standby 0.5ms で、デューティが
    // 99.6% になる。ダイが自分の熱で温まり、その温度で湿度を補正するため湿度が
    // 低めに出る。Bosch の weather monitoring 推奨（forced / ×1 / フィルタ無し）に
    // 合わせる。3 秒間隔で forced なら 1 回 ~10ms、デューティは 0.3% に落ちる。
    sensor.setSampling(Adafruit_BME280::MODE_FORCED,
                       Adafruit_BME280::SAMPLING_X1,   // 温度
                       Adafruit_BME280::SAMPLING_X1,   // 気圧
                       Adafruit_BME280::SAMPLING_X1,   // 湿度
                       Adafruit_BME280::FILTER_OFF);
    return true;
#else
    return sensor.begin();
#endif
}

// センサを実際に叩くのはここだけ。呼び出すのは loop() だけで、
// HTTP の処理からは呼ばない（AM2320 の 2.0 秒規則を外から破られないため）。
void updateSensorData() {
    bool ok;
#if SENSOR_TYPE == SENSOR_BME280
    // forced モードでは測りたい時に自分で起こす。これを呼ばないと値が更新されない。
    ok = sensor.takeForcedMeasurement();
    if (ok) {
        float t = sensor.readTemperature();
        float h = sensor.readHumidity();
        float pr = sensor.readPressure() / 100.0F;
        // データレジスタに CRC が無いので化けはここでは捕まらない（百葉箱が1ヶ月
        // もっともらしい嘘を出し続けたのはこれ）。応答が無い場合だけ NAN で出る。
        ok = !isnan(t) && !isnan(h) && !isnan(pr);
        if (ok) {
            currentTemp = t;
            currentHumidity = h;
            currentPressure = pr;
        }
    }
    if (!ok) Serial.println("BME280 read failed");
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
