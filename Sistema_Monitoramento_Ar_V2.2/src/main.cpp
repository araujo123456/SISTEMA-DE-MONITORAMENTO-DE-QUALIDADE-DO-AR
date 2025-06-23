/*******************************************************************
    ESP32 Air Quality Dashboard V13.2 (Versão Estável Final)

    Funcionalidades:
    - Leitura de sensores PMS5003 e AHT10.
    - Display TFT com dashboard de dados em tempo real.
    - Ciclo de leitura/descanso para prolongar a vida útil do PMS5003.
    - Modo Access Point (AP) para configuração de Wi-Fi e senha de admin.
    - Dashboard web principal protegido por senha.
    - Função de "Reset de Fábrica" ao segurar o botão BOOT por 10s
      na inicialização para apagar senhas e credenciais salvas.
 *******************************************************************/

// --- Bibliotecas Essenciais ---
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <PMS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// --- Mapeamento de Pinos ---
#define BOOT_BUTTON_PIN 0
#define TFT_BL_PIN 21
#define I2C_SDA_PIN 27
#define I2C_SCL_PIN 22
#define PMS_RX_PIN 19
#define PMS_TX_PIN 18

// --- Configurações do Modo AP ---
const char* AP_SSID = "Monitor de Ar Config";
const char* AP_PASSWORD = "configurar";
const byte DNS_PORT = 53;

// --- Objetos ---
Adafruit_AHTX0 aht;
PMS pms(Serial2);
PMS::DATA pms_data;
TFT_eSPI tft = TFT_eSPI();
Preferences preferences;
DNSServer dnsServer;
AsyncWebServer server(80);

// --- Variáveis Globais ---
String admin_pass = "12345678";
String current_status_text = "";
struct AirQuality { int pm1_0, pm10, pm25; float temp, hum; } current_data = {-1, -1, -1, 0.0, 0.0};
enum SensorCycleState { SENSOR_SLEEPING, SENSOR_WARMING_UP, SENSOR_READING } pms_cycle_state = SENSOR_WARMING_UP;
const unsigned long PMS_WARMUP_DURATION_MS = 30000;
const unsigned long SENSOR_ACTIVE_DURATION_MS = 30 * 60000;
const unsigned long SENSOR_SLEEP_DURATION_MS = 5 * 60000;
const unsigned long SENSOR_DATA_UPDATE_INTERVAL_MS = 5000;
unsigned long cycle_state_enter_time = 0;
unsigned long last_data_update_time = 0;
struct ColorScheme { uint16_t bg, text, status_good, status_medium, status_poor, glow; } scheme = { TFT_BLACK, TFT_WHITE, TFT_GREEN, TFT_YELLOW, TFT_RED, 0x3DDF };

// --- Páginas HTML ---
const char* config_html = R"rawliteral(
<!DOCTYPE html><html><head><title>Configuracao WiFi</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body{font-family: Arial, sans-serif; background-color: #2c3e50; color: #ecf0f1; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 20px 0;}
  .container{background-color: #34495e; padding: 20px; border-radius: 10px; box-shadow: 0 5px 15px rgba(0,0,0,0.3); text-align: center; max-width: 300px;}
  h2{color: #1abc9c;} h3{color: #f1c40f; margin-top: 30px;}
  input[type="text"], input[type="password"]{width: 90%; padding: 10px; margin: 10px 0; border: none; border-radius: 5px; background-color: #2c3e50; color: #ecf0f1;}
  input[type="submit"]{background-color: #1abc9c; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; width: 100%;}
  input[type="submit"]:hover{background-color: #16a085;}
  .pw{margin-bottom:20px;} .show-pass{font-size: 0.8em; text-align: left; width: 90%; margin: -5px auto 10px auto;}
  hr{border-color: #2c3e50;}
</style>
</head><body>
<div class="container">
  <form action="/save" method="POST">
    <h2>Configurar Wi-Fi</h2>
    <b>Rede Wi-Fi (SSID):</b><br><input type="text" name="ssid" required><br>
    <b>Senha da Rede:</b><br><input type="password" name="pass" id="wifi_pass"><br>
    <div class="show-pass"><input type="checkbox" onclick="togglePassword('wifi_pass')"> Mostrar Senha</div>
    <hr>
    <h3>Alterar Senha do Sistema</h3>
    <div class="pw"><b>Senha de Acesso Atual:</b><br><input type="password" name="admin_pass_current" required></div>
    <b>Nova Senha (min. 8 caracteres):</b><br><input type="password" name="admin_pass_new" id="admin_pass_new"><br>
    <div class="show-pass"><input type="checkbox" onclick="togglePassword('admin_pass_new')"> Mostrar Nova Senha</div>
    <input type="submit" value="Salvar e Reiniciar">
  </form>
</div>
<script>
function togglePassword(fieldId) { var x = document.getElementById(fieldId); x.type = (x.type === "password") ? "text" : "password"; }
</script>
</body></html>)rawliteral";

const char* dashboard_html = R"rawliteral(
<!DOCTYPE html><html><head><title>Air Monitor Dashboard</title><meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
    body{font-family:Arial,sans-serif;background-color:#0d1117;color:#c9d1d9;text-align:center;margin:0;padding:20px}
    h1{color:#58a6ff;border-bottom:2px solid #30363d;padding-bottom:10px}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:20px;margin-top:20px}
    .card{background-color:#161b22;border:1px solid #30363d;border-radius:10px;padding:20px;box-shadow:0 4px 8px rgba(0,0,0,.2)}
    .card.status-BOM{border-left:5px solid #28a745} .card.status-MODERADO{border-left:5px solid #ffc107}
    .card.status-RUIM{border-left:5px solid #dc3545} .card.status-descanso{border-left:5px solid #6c757d}
    h2{margin-top:0;font-size:1.2em;color:#8b949e}
    .value{font-size:2.5em;font-weight:700;margin:10px 0;color:#f0f6fc}
    .unit{font-size:1em;color:#8b949e}
    .status-text{font-size:2em;font-weight:700}
    .status-BOM .status-text{color:#28a745} .status-MODERADO .status-text{color:#ffc107}
    .status-RUIM .status-text{color:#dc3545} .status-descanso .status-text{color:#6c757d}
    footer{margin-top:30px;font-size:.8em;color:#8b949e}
</style>
</head><body>
    <h1>Monitor de Qualidade do Ar</h1>
    <div class="grid">
        <div id="status-card" class="card"><h2>Qualidade do Ar</h2><div id="status" class="status-text">---</div></div>
        <div class="card"><h2>PM1.0</h2><span id="pm1_0" class="value">---</span><span class="unit"> &micro;g/m&sup3;</span></div>
        <div class="card"><h2>PM2.5</h2><span id="pm2_5" class="value">---</span><span class="unit"> &micro;g/m&sup3;</span></div>
        <div class="card"><h2>PM10</h2><span id="pm10" class="value">---</span><span class="unit"> &micro;g/m&sup3;</span></div>
        <div class="card"><h2>Temperatura</h2><span id="temp" class="value">--.-</span><span class="unit"> &deg;C</span></div>
        <div class="card"><h2>Umidade</h2><span id="hum" class="value">--.-</span><span class="unit"> %</span></div>
    </div>
    <footer>As leituras s&atilde;o atualizadas a cada 5 segundos.</footer>
<script>
function updateData(){fetch("/data.json").then(e=>e.json()).then(e=>{document.getElementById("pm1_0").textContent=e.pm1_0,document.getElementById("pm2_5").textContent=e.pm25,document.getElementById("pm10").textContent=e.pm10,document.getElementById("temp").textContent=e.temp.toFixed(1),document.getElementById("hum").textContent=e.hum.toFixed(1);let t=document.getElementById("status"),c=document.getElementById("status-card");t.textContent=e.status,c.className="card status-"+e.status.replace(/\s+/g,"-")})}
setInterval(updateData,5e3);window.onload=updateData;
</script>
</body></html>)rawliteral";

// --- Protótipos de Funções ---
void checkFactoryReset();
void setupWifiManager();
void setupWebServer();
void drawSplashScreen();
void displayConnectionInfo();
void drawMainTab();
void manageSensorCycleAndData();
void updateDisplayValues();
void updateStatus(int pm2_5, int pm10);


void setup() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  
  tft.init();
  tft.setRotation(0);
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  checkFactoryReset();

  drawSplashScreen();
  delay(2000);

  preferences.begin("air-monitor", true);
  admin_pass = preferences.getString("admin_pass", "12345678");
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  preferences.end(); 

  WiFi.mode(WIFI_STA);
  if (ssid == "") {
    setupWifiManager();
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("Conectando ao WiFi...", 120, 160);
    WiFi.begin(ssid.c_str(), pass.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      delay(5000);
      setupWifiManager();
    } else {
      displayConnectionInfo();
      setupWebServer(); 
    }
  }

  if(WiFi.status() == WL_CONNECTED) {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Serial2.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
    pms.wakeUp();
    if(!aht.begin()){ while(1) delay(10); }
    cycle_state_enter_time = millis();
    last_data_update_time = millis() - SENSOR_DATA_UPDATE_INTERVAL_MS;
    drawMainTab();
    manageSensorCycleAndData();
  }
}

void loop() {
  if (WiFi.getMode() == WIFI_AP) {
    dnsServer.processNextRequest();
  }
  if (WiFi.getMode() == WIFI_STA) {
    manageSensorCycleAndData();
  }
  delay(10);
}

void checkFactoryReset() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Pressione BOOT para resetar", 120, 160);
  delay(2000);

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    unsigned long press_start_time = millis();
    tft.fillScreen(TFT_ORANGE);
    tft.setTextColor(TFT_BLACK);

    while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      int seconds_left = 10 - ((millis() - press_start_time) / 1000);
      if (seconds_left < 0) {
        seconds_left = 0;
      }
      
      String countdown_text = "Mantenha pressionado (" + String(seconds_left) + "s)";
      tft.fillRect(0, 150, 240, 30, TFT_ORANGE);
      tft.drawString(countdown_text, 120, 160);
      
      if (seconds_left == 0) {
        preferences.begin("air-monitor", false);
        preferences.clear();
        preferences.end();
        
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE);
        tft.setFreeFont(&FreeSansBold12pt7b);
        tft.drawString("Senhas Resetadas!", 120, 140);
        tft.setFreeFont(&FreeSans9pt7b);
        tft.drawString("Reiniciando...", 120, 180);
        Serial.println("SENHAS RESETADAS! Reiniciando...");
        delay(3000);
        ESP.restart();
      }
      delay(200);
    }
    tft.fillScreen(TFT_BLACK);
  }
}

void setupWifiManager() {
  tft.fillScreen(TFT_BLUE);
  tft.setTextDatum(TC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString("Modo Config.", 120, 30);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Conecte-se a rede:", 10, 80);
  tft.drawString("SSID: " + String(AP_SSID), 10, 100);
  tft.drawString("Senha: " + String(AP_PASSWORD), 10, 120);
  tft.drawString("\nNo navegador, acesse:", 10, 150);
  tft.drawString("IP: 192.168.4.1", 10, 190);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(200, "text/html", config_html); });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    String current_pass = request->hasParam("admin_pass_current", true) ? request->getParam("admin_pass_current", true)->value() : String();
    String new_pass = request->hasParam("admin_pass_new", true) ? request->getParam("admin_pass_new", true)->value() : String();
    String ssid_form = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : String();
    String pass_form = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : String();
    
    if(current_pass != admin_pass){ request->send(401, "text/plain", "Senha de acesso atual incorreta!"); return; }
    
    preferences.begin("air-monitor", false);
    if(ssid_form.length() > 0) {
      preferences.putString("ssid", ssid_form);
      preferences.putString("pass", pass_form);
      Serial.println("Credenciais de WiFi salvas.");
    }
    if(new_pass.length() >= 8) {
      preferences.putString("admin_pass", new_pass);
      Serial.println("Nova senha de admin salva.");
    } else if (new_pass.length() > 0) {
      request->send(400, "text/plain", "A nova senha deve ter pelo menos 8 caracteres.");
      preferences.end();
      return;
    }
    preferences.end();

    request->send(200, "text/html", "<h2>Configuracoes salvas!</h2><p>O dispositivo sera reiniciado em 5 segundos...</p>");
    delay(5000);
    ESP.restart();
  });
  server.begin();
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->authenticate("admin", admin_pass.c_str())) {
      return request->requestAuthentication();
    }
    request->send(200, "text/html", dashboard_html);
  });
  server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->authenticate("admin", admin_pass.c_str())) {
      return request->requestAuthentication();
    }
    JsonDocument doc;
    doc["pm1_0"] = (current_data.pm1_0 == -1) ? "---" : String(current_data.pm1_0);
    doc["pm25"] = (current_data.pm25 == -1) ? "---" : String(current_data.pm25);
    doc["pm10"] = (current_data.pm10 == -1) ? "---" : String(current_data.pm10);
    doc["temp"] = current_data.temp;
    doc["hum"] = current_data.hum;
    doc["status"] = current_status_text;
    String json_response;
    serializeJson(doc, json_response);
    request->send(200, "application/json", json_response);
  });
  server.begin();
}

void displayConnectionInfo() {
  tft.fillScreen(TFT_DARKGREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString("Conectado!", 120, 80);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("IP para dashboard:", 120, 130);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(WiFi.localIP().toString(), 120, 165);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Iniciando monitor...", 120, 240);
  delay(10000);
}

void drawSplashScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString("Monitor de Ar", 120, 140);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("v2.2", 120, 170);
  tft.setTextDatum(BC_DATUM);
  tft.drawString("Inicializando...", 120, 300);
}

void drawMainTab() {
  tft.fillScreen(scheme.bg);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(scheme.text, scheme.bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawRoundRect(10, 80, 220, 40, 10, scheme.glow);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("PM1.0 (ug/m3)", 120, 135);
  tft.drawString("PM10 (ug/m3)", 120, 190);
  tft.drawString("PM2.5 (ug/m3)", 120, 245);
}

void manageSensorCycleAndData() {
  unsigned long current_millis = millis();
  switch (pms_cycle_state) {
    case SENSOR_SLEEPING:
      if (current_millis - cycle_state_enter_time >= SENSOR_SLEEP_DURATION_MS) {
        pms.wakeUp(); pms_cycle_state = SENSOR_WARMING_UP; cycle_state_enter_time = current_millis;
        current_data.pm1_0 = -1; current_data.pm25 = -1; current_data.pm10 = -1;
        updateDisplayValues(); 
      }
      break;
    case SENSOR_WARMING_UP:
      if (current_millis - cycle_state_enter_time >= PMS_WARMUP_DURATION_MS) {
        pms_cycle_state = SENSOR_READING; cycle_state_enter_time = current_millis;
      }
      break;
    case SENSOR_READING:
      if (current_millis - cycle_state_enter_time >= SENSOR_ACTIVE_DURATION_MS) {
        pms.sleep(); pms_cycle_state = SENSOR_SLEEPING; cycle_state_enter_time = current_millis;
        current_data.pm1_0 = -1; current_data.pm25 = -1; current_data.pm10 = -1;
        updateDisplayValues();
      }
      break;
  }
  if (current_millis - last_data_update_time >= SENSOR_DATA_UPDATE_INTERVAL_MS) {
      sensors_event_t humidity_event, temp_event;
      if (aht.getEvent(&humidity_event, &temp_event)) {
        current_data.temp = temp_event.temperature;
        current_data.hum = humidity_event.relative_humidity;
      } else {
        current_data.temp = -99.9; current_data.hum = -99.9;
      }
      if (pms_cycle_state == SENSOR_READING) {
        pms.requestRead();
        if (pms.readUntil(pms_data, 1500)) {
          current_data.pm1_0 = pms_data.PM_AE_UG_1_0;
          current_data.pm25 = pms_data.PM_AE_UG_2_5;
          current_data.pm10 = pms_data.PM_AE_UG_10_0;
        } else {
          current_data.pm1_0 = -1; current_data.pm25 = -1; current_data.pm10 = -1;
        }
      }
      updateDisplayValues();
      last_data_update_time = current_millis;
  }
}

void updateDisplayValues() {
  updateStatus(current_data.pm25, current_data.pm10); 
  tft.setTextColor(scheme.text, scheme.bg);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.fillRect(15, 85, 210, 30, scheme.bg);
  String tempHumText = "--.-C / --.-%";
  if (current_data.temp > -50 && current_data.temp < 100 && current_data.hum >= 0 && current_data.hum <= 100) {
      tempHumText = String(current_data.temp, 1) + "C / " + String(current_data.hum, 1) + "%";
  }
  tft.drawString(tempHumText, 120, 100);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.fillRect(50, 150, 140, 30, scheme.bg);
  tft.drawString((current_data.pm1_0 == -1) ? "---" : String(current_data.pm1_0), 120, 165);
  tft.fillRect(50, 205, 140, 30, scheme.bg);
  tft.drawString((current_data.pm10 == -1) ? "---" : String(current_data.pm10), 120, 220);
  tft.fillRect(50, 260, 140, 30, scheme.bg);
  tft.drawString((current_data.pm25 == -1) ? "---" : String(current_data.pm25), 120, 275);
}

void updateStatus(int pm2_5, int pm10) { 
  uint16_t color = scheme.status_good;
  String new_status_text = "BOM";
  bool pms_data_available = (pm2_5 != -1 && pm10 != -1);
  if (!pms_data_available) {
    new_status_text = "---";
    if (current_status_text != "" && current_status_text != "---") {
        if (current_status_text == "MODERADO") color = scheme.status_medium;
        else if (current_status_text == "RUIM") color = scheme.status_poor;
    } else { color = scheme.glow; }
  } else {
    if (pm2_5 > 35 || pm10 > 50) { 
      color = scheme.status_poor; new_status_text = "RUIM";
    } else if (pm2_5 > 12 || pm10 > 20) { 
      color = scheme.status_medium; new_status_text = "MODERADO";
    }
  }
  bool statusTextChangedOrWasSleeping = (new_status_text != current_status_text) || (current_status_text == "---" && new_status_text != "---");
  if (statusTextChangedOrWasSleeping) {
     tft.fillRoundRect(10, 10, 220, 60, 10, color);
     tft.drawRoundRect(10, 10, 220, 60, 10, scheme.glow);
     tft.drawRoundRect(10, 130, 220, 50, 10, color);
     tft.drawRoundRect(10, 185, 220, 50, 10, color);
     tft.drawRoundRect(10, 240, 220, 50, 10, color);
     current_status_text = new_status_text;
  }
  tft.setTextColor(TFT_WHITE, color);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Qualidade do Ar", 120, 16);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(BC_DATUM);
  tft.drawString(current_status_text, 120, 58);
}