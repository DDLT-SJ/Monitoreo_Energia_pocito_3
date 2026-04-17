/*
 * ============================================================
 *  Monitor Energía Pocito — ESP32-C3        V2.0 (SIN BATERIA)
 * ============================================================
 *  Hardware : PCF8574 (0x20) — SDA=4, SCL=5
 *  MQTT     : HiveMQ Cloud — puerto 8883 (TLS)
 *  Topic    : Energia/Pocito
 *  Alertas  : Ntfy.sh → Monitor_Energia_Pocito
 *  Autor    : Daniel SJ
 * ============================================================
 *
 *  LIBRERIAS — instalar en Arduino IDE antes de compilar:
 *    1. PubSubClient     by Nick O'Leary       (MQTT)
 *    2. PCF8574          by Renzo Mischianti   (I2C Expander)
 *    3. WiFiManager      by tzapu              (Config WiFi por AP)
 *
 *  LOGICA DE ENTRADAS PCF8574:
 *    PIN = HIGH (1) → FALLA   🔴
 *    PIN = LOW  (0) → NORMAL  🟢
 *
 *  CONFIGURACION EN LOS 3 CELULARES:
 *    1. Instalar app "ntfy" desde Google Play
 *    2. Tocar "+" y suscribirse a: Monitor_Energia_Pocito
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <PCF8574.h>
#include <WiFiManager.h>

WiFiManager wm;

// ─────────────────────────────────────────────
//  CONFIGURACION MQTT — HiveMQ Cloud
// ─────────────────────────────────────────────
const char* MQTT_HOST      = "748863d67a8940b8957fbc4523ac7f5d.s1.eu.hivemq.cloud";
const int   MQTT_PORT      = 8883;
const char* MQTT_USER      = "Daniel_SJ";
const char* MQTT_PASSWORD  = "Ddelatorre1966";
const char* MQTT_TOPIC     = "Energia/Pocito";
const char* MQTT_CLIENT_ID = "ESP32C3_Pocito";

// ─────────────────────────────────────────────
//  CONFIGURACION NTFY.SH
// ─────────────────────────────────────────────
const char* NTFY_URL = "https://ntfy.sh/Monitor_Energia_Pocito";

// ─────────────────────────────────────────────
//  PINES I2C — ESP32-C3
// ─────────────────────────────────────────────
#define I2C_SDA 4
#define I2C_SCL 5

// ─────────────────────────────────────────────
//  LED STATUS — GPIO3 (verde)
// ─────────────────────────────────────────────
#define PIN_LED_VERDE 3

// ─────────────────────────────────────────────
//  PCF8574 — Dirección 0x20 (A0, A1, A2 en GND)
// ─────────────────────────────────────────────
PCF8574 pcf(0x20, I2C_SDA, I2C_SCL);

// ─────────────────────────────────────────────
//  CLAVES JSON — ASIGNACION P0-P7
// ─────────────────────────────────────────────
const char* PIN_KEYS[8] = {
  "termo_principal_ent",    // P0
  "termo_principal_sal",    // P1
  "protec_sobrevoltaje",    // P2
  "diferencial",            // P3
  "termo_comedor_tomas",    // P4
  "termo_comedor_ilum",     // P5
  "termo_bombas_pozo",      // P6
  "termo_bomba_pileta"      // P7
};

// ─────────────────────────────────────────────
//  NOMBRES DE DISPOSITIVOS — usados en Ntfy
// ─────────────────────────────────────────────
const char* PIN_NOMBRES[8] = {
  "Entrada Interruptor Principal",                // P0
  "Salida Interruptor Principal",                 // P1
  "Salida Interruptor Sobrevoltaje",              // P2
  "Salida Interruptor Diferencial",               // P3
  "Salida Interruptor Heladera",                  // P4
  "Salida Interruptor Cámaras",                   // P5
  "Salida Interruptor Bombas Principales",        // P6
  "Salida Interruptor Bomba Piscina"              // P7
};

// ─────────────────────────────────────────────
//  VARIABLES INTERNAS
// ─────────────────────────────────────────────
WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);

bool  lastState[8]    = {false};
bool  currentState[8] = {false};
bool  firstRead       = true;
bool  pcfOk           = false;
bool  mqttConnected   = false;

unsigned long lastPublish   = 0;
unsigned long lastReconnect = 0;

const unsigned long PUBLISH_INTERVAL   = 1000;
const unsigned long RECONNECT_INTERVAL = 5000;
const unsigned long NOTIFY_COOLDOWN    = 30000;

unsigned long lastNotify[8] = {0};

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n============================================");
  Serial.println("  Monitor Energia Pocito - ESP32-C3");
  Serial.println("  MQTT + Ntfy.sh");
  Serial.println("============================================");

  Wire.begin(I2C_SDA, I2C_SCL);
  pcf.begin();
  for (int i = 0; i < 8; i++) pcf.pinMode(i, INPUT);

  pinMode(PIN_LED_VERDE, OUTPUT);
  digitalWrite(PIN_LED_VERDE, LOW);

  Serial.print("PCF8574 (0x20)... ");
  Wire.beginTransmission(0x20);
  if (Wire.endTransmission() == 0) {
    pcfOk = true;
    Serial.println("OK");
  } else {
    Serial.println("ERROR - Verificar conexiones SDA/SCL");
  }

  conectarWiFi();
  espClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);

  delay(1000);
  enviarNtfy(
    "Sistema iniciado",
    "Monitor Energia Pocito en linea\nESP32-C3 conectado correctamente",
    "low",
    "white_check_mark"
  );
}

// ─────────────────────────────────────────────
//  LOOP PRINCIPAL
// ─────────────────────────────────────────────
void loop() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi perdido - reconectando...");
    conectarWiFi();
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (mqttConnected) {
      mqttConnected = false;
      digitalWrite(PIN_LED_VERDE, LOW);
    }
    if (now - lastReconnect >= RECONNECT_INTERVAL) {
      lastReconnect = now;
      conectarMQTT();
    }
  } else {
    mqttClient.loop();
    if (!mqttConnected) {
      mqttConnected = true;
      digitalWrite(PIN_LED_VERDE, HIGH);
    }
  }

  if (!mqttConnected) {
    unsigned long blinkTime = millis() % 1000;
    digitalWrite(PIN_LED_VERDE, (blinkTime < 500) ? HIGH : LOW);
  }

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    leerYPublicar();
  }
}

// ─────────────────────────────────────────────
//  LEER PCF8574 - DETECTAR CAMBIOS - PUBLICAR
// ─────────────────────────────────────────────
void leerYPublicar() {

  if (!pcfOk) {
    Wire.beginTransmission(0x20);
    if (Wire.endTransmission() == 0) {
      pcfOk = true;
      Serial.println("PCF8574 detectado");
    } else {
      return;
    }
  }

  // Leer los 8 pines: HIGH=1 (FALLA) / LOW=0 (NORMAL)
  for (int i = 0; i < 8; i++) {
    currentState[i] = pcf.digitalRead(i);
  }

  // Detectar cambios (ignorar primera lectura al arrancar)
  if (!firstRead) {
    for (int i = 0; i < 8; i++) {
      if (currentState[i] != lastState[i]) {
        unsigned long ahora = millis();
        Serial.printf("Cambio P%d (%s): %s\n",
          i,
          PIN_NOMBRES[i],
          currentState[i] ? "FALLA" : "NORMAL"
        );
        if (ahora - lastNotify[i] >= NOTIFY_COOLDOWN) {
          lastNotify[i] = ahora;
          notificarCambio(i, currentState[i]);
        }
      }
    }
  } else {
    firstRead = false;
    Serial.println("Primera lectura OK - monitoreo activo");
  }

  for (int i = 0; i < 8; i++) lastState[i] = currentState[i];

  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC, construirJSON().c_str(), false);
  }
}

// ─────────────────────────────────────────────
//  NOTIFICAR CAMBIO VIA NTFY.SH
// ─────────────────────────────────────────────
void notificarCambio(int pin, bool nivel) {
  String titulo, mensaje, prioridad, emoji;

  if (nivel) {
    // HIGH = 1 → FALLA
    titulo    = "FALLA DE ENERGIA POCITO";
    mensaje   = "Corte de energia en " + String(PIN_NOMBRES[pin]);
    prioridad = "default";
    emoji     = "red_circle";
  } else {
    // LOW = 0 → REPOSICION
    titulo    = "REPOSICION DE ENERGIA POCITO";
    mensaje   = "Reposicion de energia en " + String(PIN_NOMBRES[pin]);
    prioridad = "default";
    emoji     = "green_circle";
  }

  enviarNtfy(titulo, mensaje, prioridad, emoji);
}

// ─────────────────────────────────────────────
//  ENVIAR NOTIFICACION A NTFY.SH
// ─────────────────────────────────────────────
//  Prioridades: min, low, default, high, urgent
//  Tags/emojis: red_circle, green_circle, warning, white_check_mark
// ─────────────────────────────────────────────
void enviarNtfy(String titulo, String mensaje, String prioridad, String emoji) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Ntfy: sin WiFi - notificacion cancelada");
    return;
  }

  Serial.println("Ntfy -> " + titulo);

  WiFiClientSecure ntfyClient;
  ntfyClient.setInsecure();
  HTTPClient ntfyHttp;

  ntfyHttp.begin(ntfyClient, NTFY_URL);
  ntfyHttp.addHeader("Content-Type", "text/plain; charset=utf-8");
  ntfyHttp.addHeader("Title",        titulo);
  ntfyHttp.addHeader("Priority",     prioridad);
  ntfyHttp.addHeader("Tags",         emoji);

  int httpCode = ntfyHttp.POST(mensaje);

  if (httpCode == 200 || httpCode == 201) {
    Serial.println("Ntfy OK (HTTP " + String(httpCode) + ")");
  } else {
    Serial.println("Ntfy ERROR (HTTP " + String(httpCode) + ")");
  }

  ntfyHttp.end();
}

// ─────────────────────────────────────────────
//  CONSTRUIR JSON PARA MQTT
// ─────────────────────────────────────────────
String construirJSON() {
  String json = "{";
  for (int i = 0; i < 8; i++) {
    json += "\"" + String(PIN_KEYS[i]) + "\":";
    json += currentState[i] ? "1" : "0";
    if (i < 7) json += ",";
  }
  json += "}";
  return json;
}

// ─────────────────────────────────────────────
//  CONECTAR WIFI - WiFiManager
// ─────────────────────────────────────────────
void conectarWiFi() {
  wm.setConfigPortalTimeout(180);
  wm.setDebugOutput(true);

  if (!wm.autoConnect("ESP32_Pocito_Config", "config123")) {
    Serial.println("\nFallo conexion AP - reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("\nWiFi OK - IP: " + WiFi.localIP().toString());
}

// ─────────────────────────────────────────────
//  CONECTAR MQTT
// ─────────────────────────────────────────────
void conectarMQTT() {
  Serial.print("Conectando MQTT... ");
  String clientId = String(MQTT_CLIENT_ID) + "_" + String(millis() % 1000);

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("OK - Topic: " + String(MQTT_TOPIC));
    mqttConnected = true;
    digitalWrite(PIN_LED_VERDE, HIGH);
    leerYPublicar();
  } else {
    Serial.println("Error - codigo: " + String(mqttClient.state()));
    /*
     * Codigos de error PubSubClient:
     *  -4 = Timeout de conexion
     *  -3 = Conexion perdida
     *  -2 = Fallo de conexion
     *  -1 = Desconectado
     *   4 = Usuario/contrasena incorrectos
     *   5 = No autorizado
     */
  }
}
