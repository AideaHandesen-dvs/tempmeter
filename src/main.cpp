#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <TM1637Display.h>

#define CLK 5
#define DIO 2
#define I2C_SDA 8
#define I2C_SCL 9

Adafruit_BME280 bme;
TM1637Display display(CLK, DIO);
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

const uint8_t SEG_T = 0b01111000;
const uint8_t SEG_H = 0b01110100;
const uint8_t SEG_AP[] = {0b01110111, 0b01110011, 0b00000000, 0b00000000};

float currentTemp = 0;
float currentHumidity = 0;
float currentPressure = 0;
String scannedSSIDs = "";

void updateSensorData() {
    currentTemp = bme.readTemperature();
    currentHumidity = bme.readHumidity();
    currentPressure = bme.readPressure() / 100.0F;
}

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
    updateSensorData();

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
    s += ".status-label{font-size:0.85em; color:#666;}";
    s += ".status-value{font-weight:bold; color:#333;}";
    s += ".manual-input{display:none; margin-top:10px;}";
    s += ".manual-input.show{display:block;}";
    s += "</style></head><body>";

    s += "<div class='container'>";
    s += "<div class='card'>";
    s += "<h1>🌡️ ESP32-C3 Sensor</h1>";
    s += "<div class='sensor-grid'>";
    s += "<div class='sensor-item temp'><div class='sensor-value'>" + String(currentTemp, 1) + "°</div><div class='sensor-label'>温度 (℃)</div></div>";
    s += "<div class='sensor-item humid'><div class='sensor-value'>" + String(currentHumidity, 1) + "%</div><div class='sensor-label'>湿度</div></div>";
    s += "<div class='sensor-item press'><div class='sensor-value'>" + String(currentPressure, 0) + "</div><div class='sensor-label'>気圧 (hPa)</div></div>";
    s += "</div></div>";

    s += "<div class='card'>";
    if (WiFi.status() == WL_CONNECTED) {
        s += "<div class='status'>";
        s += "<div class='status-label'>接続中</div>";
        s += "<div class='status-value'>" + WiFi.SSID() + "</div>";
        s += "<div class='status-label'>IP: " + WiFi.localIP().toString() + "</div>";
        s += "</div>";
    } else {
        s += "<div class='status disconnected'>";
        s += "<div class='status-label'>APモード</div>";
        s += "<div class='status-value'>ESP32C3-Setup</div>";
        s += "<div class='status-label'>IP: 192.168.4.1</div>";
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
        display.setSegments(SEG_AP);
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

    display.setBrightness(0x05);
    display.showNumberDec(8888);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!bme.begin(0x76, &Wire)) {
        Serial.println("BME280 not found!");
    } else {
        Serial.println("BME280 OK");
    }

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

    server.on("/api/data", HTTP_GET, []() {
        updateSensorData();
        String json = "{\"temperature\":" + String(currentTemp, 1) + ",\"humidity\":" + String(currentHumidity, 1) + ",\"pressure\":" + String(currentPressure, 1) + ",\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "192.168.4.1") + "\"}";
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
    static bool showTemp = true;
    if (millis() - lastUpdate > 2000) {
        if (WiFi.getMode() != WIFI_AP) {
            updateSensorData();
            updateDisplay(showTemp ? 't' : 'h', showTemp ? currentTemp : currentHumidity);
            showTemp = !showTemp;
        }
        lastUpdate = millis();
    }
}
