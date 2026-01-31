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

String makeHTML() {
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0F;

    String s = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<style>body{font-family:sans-serif; background:#f4f4f4; text-align:center; padding:15px;}";
    s += ".card{background:white; border-radius:15px; padding:20px; box-shadow:0 4px 6px rgba(0,0,0,0.1); margin-bottom:15px;}";
    s += ".val{font-size:1.4em; font-weight:bold; margin:8px 0;}";
    s += ".pass-container{position:relative; width:100%; margin:10px 0;}";
    s += "input{font-size:1.1em; width:100%; padding:12px; border-radius:8px; border:1px solid #ccc; box-sizing:border-box;}";
    s += ".toggle-btn{position:absolute; right:12px; top:50%; transform:translateY(-50%); cursor:pointer; font-size:1.2em;}";
    s += ".submit-btn{background:#007bff; color:white; border:none; padding:15px; width:100%; border-radius:8px; font-size:1.1em; cursor:pointer;}";
    s += "</style></head><body>";
    
    s += "<div class='card'><h1>ESP32-C3 Sensor</h1>";
    s += "<p class='val'>温度: " + String(t, 1) + " ℃</p>";
    s += "<p class='val'>湿度: " + String(h, 1) + " %</p>";
    s += "<p class='val'>気圧: " + String(p, 1) + " hPa</p></div>";
    
    s += "<div class='card'><h3>WiFi設定</h3><form action='/save' method='get'>";
    s += "<input name='s' placeholder='SSID' required><br>";
    s += "<div class='pass-container'><input name='p' id='pass' type='password' placeholder='Password'>";
    s += "<span class='toggle-btn' onclick='tPass()'>👁</span></div>";
    s += "<input type='submit' class='submit-btn' value='設定を記録'></form></div>";
    
    s += "<script>function tPass(){var p=document.getElementById('pass');var b=document.querySelector('.toggle-btn');if(p.type==='password'){p.type='text';b.innerText='🔒';}else{p.type='password';b.innerText='👁';}}</script>";
    s += "</body></html>";
    return s;
}

void startAP() {
    Serial.println("Starting AP Mode...");
    
    // 完全にWiFiをリセット
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    
    // APモードに設定
    WiFi.mode(WIFI_AP);
    delay(200);
    
    // SuperMini用: 送信電力を制限して電源安定化
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    delay(100);
    
    // IPアドレス設定
    IPAddress apIP(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, subnet);
    delay(100);
    
    // AP起動
    bool apStarted = WiFi.softAP("ESP32C3-Setup", "", 1, false, 4);
    
    if (apStarted) {
        Serial.println("=== AP Started ===");
        Serial.print("SSID: ESP32C3-Setup  CH: ");
        Serial.println(WiFi.channel());
        Serial.print("IP: ");
        Serial.println(WiFi.softAPIP());
        Serial.print("TxPower: ");
        Serial.println(WiFi.getTxPower());
        display.setSegments(SEG_AP);
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

    // 起動時にWiFiを完全リセット
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    
    // SuperMini用: 送信電力を制限
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    // Preferencesから読み出し
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

    server.on("/", []() { server.send(200, "text/html", makeHTML()); });

    server.on("/save", []() {
        String ssid = server.arg("s");
        String pass = server.arg("p");
        
        prefs.begin("wifi-store", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        
        String res = "<html><body style='text-align:center;padding-top:50px;font-family:sans-serif;'>";
        res += "<h2>設定を記録しました</h2>";
        res += "<p>SSID: " + ssid + "</p>";
        res += "<p style='color:red;font-weight:bold;'>本体の電源を入れ直してください。</p>";
        res += "</body></html>";
        server.send(200, "text/html", res);
        
        Serial.println("Config Saved.");
    });

    server.on("/reset", []() {
        prefs.begin("wifi-store", false);
        prefs.clear();
        prefs.end();
        server.send(200, "text/plain", "Cleared. Restarting...");
        delay(1000);
        ESP.restart();
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
            float val = showTemp ? bme.readTemperature() : bme.readHumidity();
            updateDisplay(showTemp ? 't' : 'h', val);
            showTemp = !showTemp;
        }
        lastUpdate = millis();
    }
}
