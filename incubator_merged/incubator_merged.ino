// ====================================================================
// MERGED FIRMWARE — NODE 1 + NODE 2
// Board  : LILYGO TTGO T-Display ESP32 (or ESP32 DevKit)
// Core 1 : Node 1 — Climate Control (DHT22, Relay Fan, Relay Lamp, Servo)
// Core 0 : Node 2 — Sensor Monitor  (DHT22, MQ-135, LDR)
// v1.5 : Added egg type selection (chicken/duck/quail/goose)
//
// ── PIN SUMMARY ───────────────────────────────────────────────────
//  NODE 1 (Core 1)
//    GPIO 21  → DHT22 data
//    GPIO 26  → Relay IN1 (Fan,  active-low)
//    GPIO 27  → Relay IN2 (Lamp, active-low)
//    GPIO 13  → Servo PWM
//  NODE 2 (Core 0)
//    GPIO 22  → DHT22 data
//    GPIO 32  → MQ-135 AOUT
//    GPIO 17  → MQ-135 DOUT
//    GPIO 33  → LDR analog
// ====================================================================

#include <Arduino.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ====================================================================
//  SHARED CONFIGURATION  ← CHANGE YOUR WIFI HERE
// ====================================================================
const char* WIFI_SSID     = "Khalifa";
const char* WIFI_PASSWORD = "12345678";
const char* DASHBOARD_URL = "http://192.168.95.198:8080";

SemaphoreHandle_t http_mutex;
SemaphoreHandle_t serial_mutex;

#define SERIAL_PRINT(...)   do { xSemaphoreTake(serial_mutex, portMAX_DELAY); \
                                 Serial.printf(__VA_ARGS__);                   \
                                 xSemaphoreGive(serial_mutex); } while(0)

struct AnalogStats { int avg, mn, mx; };
void servoTask(void* pv);

// ====================================================================
//  NODE 1 — CLIMATE CONTROL
// ====================================================================

#define N1_PIN_DHT          21
#define N1_PIN_RELAY_FAN    27   // Corrected: physical fan relay is on GPIO 27
#define N1_PIN_RELAY_LAMP   26   // Corrected: physical lamp relay is on GPIO 26
#define N1_PIN_SERVO        13

// ── Fixed danger thresholds (never change) ────────────────────────
#define N1_TEMP_DANGER_LOW   35.0f
#define N1_TEMP_DANGER_HIGH  39.5f   // hard upper limit regardless of egg

// ── Humidity thresholds ───────────────────────────────────────────
#define N1_HUM_LOW_DANGER    38.0f
#define N1_HUM_LOW_WARN      45.0f
#define N1_HUM_HIGH_WARN     65.0f
#define N1_HUM_HIGH_DANGER   75.0f

// ── Servo ─────────────────────────────────────────────────────────
#define N1_SERVO_POS_CENTRE  90
#define N1_SERVO_POS_CW     120
#define N1_SERVO_POS_CCW     60   
#define N1_SERVO_SWEEP_MS   50UL
#define N1_SERVO_DWELL_MS   5000UL

// ── Timing ───────────────────────────────────────────────────────
#define N1_READ_INTERVAL_MS     3000UL
#define N1_HTTP_INTERVAL_MS     3000UL
#define N1_CMD_POLL_INTERVAL_MS 3000UL
#define N1_INCUBATION_DAYS      21

// ── Egg profiles ─────────────────────────────────────────────────
struct EggProfile {
  const char* name;
  float temp_target;
  float temp_cold;   // lamp ON below this
  float temp_hot;    // fan ON above this
  int incubation_days;
};

// DEFAULT = chicken (index 0)
static const EggProfile EGG_PROFILES[] = {
  { "chicken", 37.5f, 36.5f, 38.5f, 21 },
  { "duck",    37.7f, 37.0f, 39.0f, 28 },
  { "quail",   37.8f, 37.5f, 39.5f, 18 },
  { "goose",   37.6f, 36.8f, 38.8f, 25 },
};
#define EGG_COUNT 4

// ── Lamp enum ─────────────────────────────────────────────────────
enum N1_LampMode { LAMP_OFF, LAMP_FULL };

// ── Node 1 state ──────────────────────────────────────────────────
struct Node1State {
  DHT*         dht;
  Servo*       servo;

  N1_LampMode  lamp_mode           = LAMP_OFF;
  bool         fan_on              = false;
  bool         sensor_ok           = true;
  float        last_temp           = NAN;
  float        last_hum            = NAN;

  // Egg type (default chicken)
  int          egg_index           = 0;          // index into EGG_PROFILES
  float        temp_cold           = 36.5f;      // dynamic, updated on egg change
  float        temp_hot            = 38.5f;      // dynamic

  // Servo
  int          servo_state         = 0;
  int          servo_target_state  = 0;
  int          servo_pos           = N1_SERVO_POS_CENTRE;
  int          servo_start_pos     = N1_SERVO_POS_CENTRE;
  bool         servo_moving        = false;
  unsigned long servo_move_start   = 0;
  unsigned long last_servo_move    = 0;
  int          servo_turn_count    = 0;
  unsigned long servo_sweep_ms     = N1_SERVO_SWEEP_MS;
  unsigned long servo_dwell_ms     = N1_SERVO_DWELL_MS;

  int          read_cycle          = 0;
  int          dht_fails           = 0;
  int          dht_total           = 0;

  char         control_mode[16]    = "AUTO";
  bool         manual_fan          = false;
  N1_LampMode  manual_lamp         = LAMP_OFF;
  bool         manual_servo_trigger = false;
  unsigned long manual_active_since = 0;

  unsigned long last_read          = 0;
  unsigned long last_http          = 0;
  unsigned long last_cmd_poll      = 0;
  unsigned long incubation_start   = 0;

  bool         wifi_ok             = false;
};

// ── Helper: find egg profile index by name ─────────────────────
static int findEggIndex(const char* name) {
  for (int i = 0; i < EGG_COUNT; i++) {
    if (strcasecmp(EGG_PROFILES[i].name, name) == 0) return i;
  }
  return 0; // default chicken
}

// ── Helper: apply egg profile thresholds to state ─────────────
static void applyEggProfile(Node1State& s, int idx) {
  s.egg_index = idx;
  s.temp_cold = EGG_PROFILES[idx].temp_cold;
  s.temp_hot  = EGG_PROFILES[idx].temp_hot;
  SERIAL_PRINT("[N1-EGG] Profile: %s  cold=%.1f  hot=%.1f\n",
               EGG_PROFILES[idx].name, s.temp_cold, s.temp_hot);
}

// ── Actuator helpers (active-HIGH: NC wiring — HIGH = device ON) ──
static void n1_setFan(Node1State& s, bool on) {
  s.fan_on = on;
  digitalWrite(N1_PIN_RELAY_FAN, on ? HIGH : LOW);
}

static void n1_setLamp(Node1State& s, N1_LampMode mode) {
  s.lamp_mode = mode;
  digitalWrite(N1_PIN_RELAY_LAMP, (mode == LAMP_FULL) ? HIGH : LOW);
}

// ── Servo task ────────────────────────────────────────────────
void servoTask(void* pv) {
  Node1State* s = (Node1State*)pv;

  Servo servo_obj;
  s->servo = &servo_obj;

  ESP32PWM::allocateTimer(0);
  servo_obj.setPeriodHertz(50);
  
  // Start attached to set initial position, then detach to save power and stop buzzing
  servo_obj.attach(N1_PIN_SERVO, 500, 2400);
  servo_obj.write(N1_SERVO_POS_CENTRE);
  vTaskDelay(pdMS_TO_TICKS(500));
  servo_obj.detach();

  s->last_servo_move = millis();
  SERIAL_PRINT("[SERVO-TASK] Initialized on Core %d\n", xPortGetCoreID());

  for (;;) {
    unsigned long now = millis();

    if (s->manual_servo_trigger) {
      s->last_servo_move = 0;
      s->manual_servo_trigger = false;
      SERIAL_PRINT("[SERVO-TASK] Manual trigger\n");
    }

    if (s->servo_moving) {
      unsigned long elapsed = now - s->servo_move_start;
      if (elapsed >= s->servo_sweep_ms) {
        s->servo->write(s->servo_pos);
        
        // Wait 150ms for the servo to reach final position, then detach to eliminate buzzing
        vTaskDelay(pdMS_TO_TICKS(150));
        s->servo->detach();
        
        s->servo_moving = false;
        s->servo_state = s->servo_target_state;
        s->last_servo_move = millis();
        s->servo_turn_count++;
        SERIAL_PRINT("[SERVO-TASK] Reached %d deg (turn #%d) and detached\n",
                     s->servo_pos, s->servo_turn_count);
      } else {
        float progress = (float)elapsed / (float)s->servo_sweep_ms;
        int current_pos = s->servo_start_pos + (int)((s->servo_pos - s->servo_start_pos) * progress);
        s->servo->write(current_pos);
      }
    } else {
      if (now - s->last_servo_move >= s->servo_dwell_ms) {
        s->servo_start_pos = s->servo_pos;
        s->servo_target_state = (s->servo_state + 1) % 4;
        switch (s->servo_target_state) {
          case 0: s->servo_pos = N1_SERVO_POS_CENTRE; break;
          case 1: s->servo_pos = N1_SERVO_POS_CW;     break;
          case 2: s->servo_pos = N1_SERVO_POS_CENTRE; break;
          case 3: s->servo_pos = N1_SERVO_POS_CCW;    break;
        }
        
        // Dynamically attach the servo before starting the sweep
        if (!s->servo->attached()) {
          s->servo->attach(N1_PIN_SERVO, 500, 2400);
        }
        
        s->servo_move_start = millis();
        s->servo_moving     = true;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ── AUTO regulation (uses dynamic thresholds from egg profile) ─
static void n1_autoRegulate(Node1State& s, float t) {
  float target = EGG_PROFILES[s.egg_index].temp_target;
  if (t >= N1_TEMP_DANGER_HIGH) {
    n1_setLamp(s, LAMP_OFF); n1_setFan(s, true);
    SERIAL_PRINT("[N1-DANGER] %.1f >= %.1f EMERGENCY FAN\n", t, N1_TEMP_DANGER_HIGH);
  } else if (t <= N1_TEMP_DANGER_LOW) {
    n1_setLamp(s, LAMP_FULL); n1_setFan(s, false);
    SERIAL_PRINT("[N1-DANGER] %.1f <= %.1f EMERGENCY LAMP\n", t, N1_TEMP_DANGER_LOW);
  } else {
    // Standard Hysteresis Control: ±0.5 degrees from target
    // 1. Lamp Control (Heating): Opens (ON) at target - 0.5°C, closes (OFF) at target + 0.5°C
    if (t <= target - 0.5f) {
      n1_setLamp(s, LAMP_FULL);
    } else if (t >= target + 0.5f) {
      n1_setLamp(s, LAMP_OFF);
    }
    
    // 2. Fan Control (Cooling): ON at target + 0.5°C, OFF at target - 0.5°C
    if (t >= target + 0.5f) {
      n1_setFan(s, true);
    } else if (t <= target - 0.5f) {
      n1_setFan(s, false);
    }
    
    SERIAL_PRINT("[N1-AUTO] Target:%.1f  Lamp:%s  Fan:%s\n", 
                 target, 
                 s.lamp_mode == LAMP_FULL ? "ON" : "OFF", 
                 s.fan_on ? "ON" : "OFF");
  }
}

// ── Node 1 FreeRTOS Task ──────────────────────────────────────
void node1Task(void* pv) {
  Node1State s;

  DHT dht_obj(N1_PIN_DHT, DHT22);
  s.dht = &dht_obj;

  pinMode(N1_PIN_RELAY_FAN,  OUTPUT);
  pinMode(N1_PIN_RELAY_LAMP, OUTPUT);
  n1_setFan(s, false);
  n1_setLamp(s, LAMP_OFF);

  // Apply default egg profile (chicken) on boot
  applyEggProfile(s, 0);

  dht_obj.begin();
  s.incubation_start = millis();

  SERIAL_PRINT("[N1] Relay self-test...\n");
  n1_setFan(s, true);  vTaskDelay(pdMS_TO_TICKS(600)); n1_setFan(s, false);
  n1_setLamp(s, LAMP_FULL); vTaskDelay(pdMS_TO_TICKS(600)); n1_setLamp(s, LAMP_OFF);

  xTaskCreatePinnedToCore(servoTask, "Servo", 4096, &s, 2, NULL, 1);

  SERIAL_PRINT("[N1] Node 1 running on Core %d, Egg: %s\n",
               xPortGetCoreID(), EGG_PROFILES[s.egg_index].name);

  for (;;) {
    unsigned long now = millis();
    s.wifi_ok = (WiFi.status() == WL_CONNECTED);

    // ── Sensor read every 3s ────────────────────────────────
    if (now - s.last_read >= N1_READ_INTERVAL_MS) {
      s.last_read = now;
      s.read_cycle++;

      s.dht_total++;
      float t = s.dht->readTemperature();
      float h = s.dht->readHumidity();

      if (isnan(t) || isnan(h)) {
        s.dht_fails++;
        s.sensor_ok = false;
        SERIAL_PRINT("[N1-DHT] FAIL (%d/%d)\n", s.dht_fails, s.dht_total);
        n1_setLamp(s, LAMP_OFF);
        n1_setFan(s, false);
      } else {
        s.sensor_ok = true;
        s.last_temp = t;
        s.last_hum  = h;
        SERIAL_PRINT("[N1-DHT] Temp:%.1fC  Hum:%.1f%%  Egg:%s\n",
                     t, h, EGG_PROFILES[s.egg_index].name);

        bool is_manual = (strcmp(s.control_mode, "MANUAL") == 0);

        // Auto-revert MANUAL after 15 min
        if (is_manual && (now - s.manual_active_since) > 900000UL) {
          strcpy(s.control_mode, "AUTO");
          is_manual = false;
          SERIAL_PRINT("[N1] MANUAL timeout - reverted to AUTO\n");
        }

        // Danger zones always override manual
        if (t >= N1_TEMP_DANGER_HIGH || t <= N1_TEMP_DANGER_LOW) {
          n1_autoRegulate(s, t);
        } else if (is_manual) {
          n1_setFan(s, s.manual_fan);
          n1_setLamp(s, s.manual_lamp);
          SERIAL_PRINT("[N1-MANUAL] Fan:%s Lamp:%s\n",
                       s.manual_fan ? "ON" : "OFF",
                       s.manual_lamp == LAMP_FULL ? "FULL" : "OFF");
        } else {
          n1_autoRegulate(s, t);
        }
      }
    }

    // ── HTTP POST every 3s ──────────────────────────────────
    if (s.wifi_ok && (now - s.last_http >= N1_HTTP_INTERVAL_MS)) {
      s.last_http = now;

      xSemaphoreTake(http_mutex, portMAX_DELAY);
      HTTPClient http;
      http.setTimeout(2500);
      http.begin(String(DASHBOARD_URL) + "/api/data");
      http.addHeader("Content-Type", "application/json");

      unsigned long day = (now - s.incubation_start) / 86400000UL + 1;
      int max_days = EGG_PROFILES[s.egg_index].incubation_days;
      if (day > max_days) day = max_days;

      JsonDocument doc;
      doc["node"]           = 1;
      if (isnan(s.last_temp)) doc["temperature"] = nullptr;
      else                    doc["temperature"] = s.last_temp;
      if (isnan(s.last_hum))  doc["humidity"]    = nullptr;
      else                    doc["humidity"]    = s.last_hum;
      doc["fan"]            = s.fan_on;
      doc["lamp"]           = (s.lamp_mode == LAMP_FULL) ? "FULL" : "OFF";
      doc["servo_angle"]    = s.servo_pos;
      doc["servo_turns"]    = s.servo_turn_count;
      doc["incubation_day"] = (int)day;
      doc["sensor_ok"]      = s.sensor_ok;
      doc["cycle"]          = s.read_cycle;
      doc["mode"]           = s.control_mode;
      doc["egg_type"]       = EGG_PROFILES[s.egg_index].name;

      String body;
      serializeJson(doc, body);
      int code = http.POST(body);
      SERIAL_PRINT("[N1-HTTP] POST → %d\n", code);
      if (code == 200) {
        String response = http.getString();
        JsonDocument resDoc;
        DeserializationError err = deserializeJson(resDoc, response);
        if (!err && resDoc.containsKey("egg_type")) {
          String eggType = resDoc["egg_type"].as<String>();
          int serverIdx = findEggIndex(eggType.c_str());
          if (serverIdx != s.egg_index) {
            applyEggProfile(s, serverIdx);
            SERIAL_PRINT("[N1-SYNC] Synced to server egg type: %s\n", eggType.c_str());
          }
        }
      }
      http.end();
      xSemaphoreGive(http_mutex);
    }

    // ── Command poll every 3s ───────────────────────────────
    if (s.wifi_ok && (now - s.last_cmd_poll >= N1_CMD_POLL_INTERVAL_MS)) {
      s.last_cmd_poll = now;

      xSemaphoreTake(http_mutex, portMAX_DELAY);
      HTTPClient http;
      http.setTimeout(2500);
      http.begin(String(DASHBOARD_URL) + "/api/commands/1");
      int code = http.GET();
      if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        deserializeJson(doc, payload);
        JsonArray cmds = doc["commands"].as<JsonArray>();
        for (JsonObject cmd : cmds) {
          String command = cmd["command"].as<String>();

          if (command == "mode") {
            String m = cmd["value"].as<String>();
            if (m == "AUTO" || m == "MANUAL") {
              strncpy(s.control_mode, m.c_str(), sizeof(s.control_mode) - 1);
              if (m == "MANUAL") {
                s.manual_active_since = millis();
                // Snapshot current actuator states so switching to MANUAL
                // preserves whatever AUTO was doing (lamp stays on, fan stays off, etc.)
                s.manual_fan  = s.fan_on;
                s.manual_lamp = s.lamp_mode;
                SERIAL_PRINT("[N1-CMD] Mode → MANUAL (snapshot: fan=%s lamp=%s)\n",
                             s.fan_on ? "ON" : "OFF",
                             s.lamp_mode == LAMP_FULL ? "FULL" : "OFF");
              } else {
                SERIAL_PRINT("[N1-CMD] Mode → AUTO\n");
              }
            }
          }
          else if (command == "fan") {
            s.manual_fan = cmd["value"].as<bool>();
            // Apply immediately — don't wait for next sensor read
            if (strcmp(s.control_mode, "MANUAL") == 0) {
              n1_setFan(s, s.manual_fan);
              SERIAL_PRINT("[N1-CMD] Fan → %s (applied now)\n", s.manual_fan ? "ON" : "OFF");
            }
          }
          else if (command == "lamp") {
            String v = cmd["value"].as<String>();
            s.manual_lamp = (v == "FULL") ? LAMP_FULL : LAMP_OFF;
            // Apply immediately — don't wait for next sensor read
            if (strcmp(s.control_mode, "MANUAL") == 0) {
              n1_setLamp(s, s.manual_lamp);
              SERIAL_PRINT("[N1-CMD] Lamp → %s (applied now)\n", v.c_str());
            }
          }
          else if (command == "servo") {
            s.manual_servo_trigger = true;
          }
          else if (command == "egg_type") {
            // ── NEW: egg type selection ──────────────────────
            String eggName = cmd["value"].as<String>();
            int idx = findEggIndex(eggName.c_str());
            applyEggProfile(s, idx);
          }
          else if (command == "sweep") {
            s.servo_sweep_ms = cmd["value"].as<unsigned long>();
          }
          else if (command == "dwell") {
            s.servo_dwell_ms = cmd["value"].as<unsigned long>();
          }
        }
      }
      http.end();
      xSemaphoreGive(http_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ====================================================================
//  NODE 2 — SENSOR MONITOR (unchanged)
// ====================================================================

#define N2_PIN_DHT22       22
#define N2_PIN_MQ135_AOUT  32
#define N2_PIN_MQ135_DOUT  17
#define N2_PIN_LDR         33

#define N2_TEMP_DANGER_LOW   35.0f
#define N2_TEMP_COLD         36.5f
#define N2_TEMP_HOT          38.5f
#define N2_TEMP_DANGER_HIGH  39.0f

#define N2_HUM_LOW_DANGER    38.0f
#define N2_HUM_LOW_WARN      45.0f
#define N2_HUM_HIGH_WARN     65.0f
#define N2_HUM_HIGH_DANGER   75.0f

#define N2_MQ135_WARN        800
#define N2_MQ135_DANGER      1200
#define N2_MQ135_JITTER_MAX  150

#define N2_LIGHT_WARN_HIGH   60
#define N2_LIGHT_DANGER_HIGH 85

#define N2_READ_INTERVAL_MS  3000UL
#define N2_HTTP_INTERVAL_MS  3000UL
#define N2_ADC_SAMPLES       5
#define N2_INCUBATION_DAYS   21

static AnalogStats n2_readADC(uint8_t pin, int n) {
  uint32_t sum = 0;
  int vmin = 4095, vmax = 0;
  for (int i = 0; i < n; i++) {
    int v = analogRead(pin);
    sum += (uint32_t)v;
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
    delayMicroseconds(100);
  }
  return { (int)(sum / (uint32_t)n), vmin, vmax };
}

struct Node2State {
  DHT* dht;
  float s_temp      = NAN;
  float s_hum       = NAN;
  int   s_mq_raw    = 0;
  float s_mq_volts  = 0.0f;
  int   s_mq_dout   = HIGH;
  int   s_light_pct = 0;
  bool  sensor_ok   = true;
  int   read_cycle  = 0;
  int   dht_fails   = 0;
  int   dht_total   = 0;
  unsigned long last_read        = 0;
  unsigned long last_http        = 0;
  unsigned long incubation_start = 0;
  bool  wifi_ok     = false;
};

void node2Task(void* pv) {
  Node2State s;
  DHT dht_obj(N2_PIN_DHT22, DHT22);
  s.dht = &dht_obj;

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(N2_PIN_MQ135_AOUT, ADC_11db);
  analogSetPinAttenuation(N2_PIN_LDR, ADC_11db);
  pinMode(N2_PIN_MQ135_DOUT, INPUT);

  dht_obj.begin();
  s.incubation_start = millis();

  SERIAL_PRINT("[N2] Node 2 running on Core %d\n", xPortGetCoreID());

  for (;;) {
    unsigned long now = millis();
    s.wifi_ok = (WiFi.status() == WL_CONNECTED);

    if (now - s.last_read >= N2_READ_INTERVAL_MS) {
      s.last_read = now;
      s.read_cycle++;

      s.dht_total++;
      float t = s.dht->readTemperature();
      float h = s.dht->readHumidity();

      if (isnan(t) || isnan(h)) {
        s.dht_fails++;
        s.sensor_ok = false;
        SERIAL_PRINT("[N2-DHT] FAIL (%d/%d)\n", s.dht_fails, s.dht_total);
      } else {
        s.sensor_ok = true;
        s.s_temp = t;
        s.s_hum  = h;
        SERIAL_PRINT("[N2-DHT] Temp:%.1fC  Hum:%.1f%%\n", t, h);
        if (t >= N2_TEMP_DANGER_HIGH)
          SERIAL_PRINT("[N2-DANGER] Temp %.1fC - LETHAL!\n", t);
        else if (t <= N2_TEMP_DANGER_LOW)
          SERIAL_PRINT("[N2-DANGER] Temp %.1fC - ARREST RISK!\n", t);
      }

      AnalogStats mq = n2_readADC(N2_PIN_MQ135_AOUT, N2_ADC_SAMPLES);
      s.s_mq_raw   = mq.avg;
      s.s_mq_volts = s.s_mq_raw * (3.3f / 4095.0f);
      s.s_mq_dout  = digitalRead(N2_PIN_MQ135_DOUT);
      SERIAL_PRINT("[N2-MQ135] ADC:%d (%.2fV) DOUT:%s\n",
                   s.s_mq_raw, s.s_mq_volts,
                   s.s_mq_dout == LOW ? "GAS!" : "clear");
      if (s.s_mq_dout == LOW || s.s_mq_raw >= N2_MQ135_DANGER)
        SERIAL_PRINT("[N2-DANGER] HIGH AMMONIA!\n");

      AnalogStats ldr = n2_readADC(N2_PIN_LDR, N2_ADC_SAMPLES);
      s.s_light_pct = map(ldr.avg, 0, 4095, 0, 100);
      SERIAL_PRINT("[N2-LDR] ADC:%d  Light:%d%%\n", ldr.avg, s.s_light_pct);
    }

    if (s.wifi_ok && (now - s.last_http >= N2_HTTP_INTERVAL_MS)) {
      s.last_http = now;

      xSemaphoreTake(http_mutex, portMAX_DELAY);
      HTTPClient http;
      http.setTimeout(2500);
      http.begin(String(DASHBOARD_URL) + "/api/data");
      http.addHeader("Content-Type", "application/json");

      unsigned long day = (now - s.incubation_start) / 86400000UL + 1;
      if (day > N2_INCUBATION_DAYS) day = N2_INCUBATION_DAYS;

      JsonDocument doc;
      doc["node"]        = 2;
      if (isnan(s.s_temp)) doc["temperature"] = nullptr;
      else                 doc["temperature"] = s.s_temp;
      if (isnan(s.s_hum))  doc["humidity"]    = nullptr;
      else                 doc["humidity"]    = s.s_hum;
      doc["mq135_raw"]   = s.s_mq_raw;
      doc["mq135_volts"] = s.s_mq_volts;
      doc["mq135_dout"]  = s.s_mq_dout;
      doc["light_pct"]   = s.s_light_pct;
      doc["sensor_ok"]   = s.sensor_ok;
      doc["cycle"]       = s.read_cycle;
      doc["incubation_day"] = (int)day;

      String body;
      serializeJson(doc, body);
      int code = http.POST(body);
      SERIAL_PRINT("[N2-HTTP] POST → %d\n", code);
      http.end();
      xSemaphoreGive(http_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ====================================================================
//  SETUP + WIFI
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== INCUBATOR MERGED FIRMWARE v1.5 ===");
  Serial.println("Node 1 (Core 1): Climate Control + Egg Profiles");
  Serial.println("Node 2 (Core 0): Sensor Monitor\n");

  http_mutex   = xSemaphoreCreateMutex();
  serial_mutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(400);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Connected — IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Not connected — running standalone");
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  esp_task_wdt_config_t wdt_cfg = {};
  wdt_cfg.timeout_ms   = 10000;
  wdt_cfg.idle_core_mask = 0;
  wdt_cfg.trigger_panic  = true;
  esp_task_wdt_init(&wdt_cfg);
#else
  esp_task_wdt_init(10, true);
#endif

  xTaskCreatePinnedToCore(node1Task, "Node1", 10240, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(node2Task, "Node2", 10240, NULL, 1, NULL, 0);

  Serial.println("[MAIN] Both tasks launched.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(5000));
}
