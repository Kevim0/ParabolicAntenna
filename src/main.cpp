#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_wifi.h>

// false = USB Serial | true = UART hardware (GPIO16=RX, GPIO17=TX)
bool UART_CON = false;

#define UART_RX_PIN 16
#define UART_TX_PIN 17
#define UART_BAUD 115200

// Stepper pins
#define STEP_PAN_PIN 25
#define DIR_PAN_PIN 26
#define STEP_TILT_PIN 32
#define DIR_TILT_PIN 33
#define DIR_SETUP_DELAY 100 // us between DIR change and first STEP
#define STEPS_PER_REV 1600

enum StepPhase { PHASE_DIR_SETUP, PHASE_STEP_HIGH, PHASE_STEP_LOW };

struct StepperState {
  int stepsRemaining;
  bool clockwise;
  unsigned int stepDelayUs; // time per half-step in us (200–1000)
  unsigned long phaseStartUs;
  StepPhase phase;
  bool active;
  long position; // accumulated steps (+CW / -CCW)
};

StepperState panStepper = {0, true, 200, 0, PHASE_DIR_SETUP, false, 0};
StepperState tiltStepper = {0, true, 200, 0, PHASE_DIR_SETUP, false, 0};

struct StepperPins {
  uint8_t step;
  uint8_t dir;
};
StepperPins panPins = {STEP_PAN_PIN, DIR_PAN_PIN};
StepperPins tiltPins = {STEP_TILT_PIN, DIR_TILT_PIN};

// Fan: PWM=GPIO27 (25kHz, 8-bit), TACH=GPIO34 (input-only, 2 pulses/rev)
#define FAN_TACH_PIN 34
#define FAN_PWM_PIN 27

volatile unsigned long pulses = 0;
unsigned long lastRpmTime = 0;
float currentRpm = 0;
int currentPwmValue = 128;

void IRAM_ATTR countPulse() {
  static unsigned long lastPulseTime = 0;
  unsigned long t = micros();
  if (t - lastPulseTime > 500) {
    pulses++;
    lastPulseTime = t;
  }
}

// WiFi async connection state
enum WifiConnState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
WifiConnState wifiConnState = WIFI_IDLE;
unsigned long wifiConnStart = 0;
unsigned long uartInitTime = 0;
#define WIFI_CONN_TIMEOUT_MS 10000

// ── Communication abstraction ────────────────────────────────────────────────
void handleJsonCommand(const String &jsonString); // forward declaration

void sendResponse(const String &msg) {
  if (UART_CON)
    Serial2.println(msg);
  else
    Serial.println(msg);
}

bool dataAvailable() {
  return UART_CON ? Serial2.available() > 0 : Serial.available() > 0;
}

void initComm() {
  if (UART_CON) {
    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    uartInitTime = millis();
  }
}

// ── Radio management ─────────────────────────────────────────────────────────
void shutdownWifi() {
  if (WiFi.status() == WL_CONNECTED)
    WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  wifiConnState = WIFI_IDLE;
}

// ── Tick functions (non-blocking, called every loop iteration) ───────────────

String serialBuffer = "";
static bool readyPrinted = false;
static unsigned long setupTime = 0;

void tickSerial() {
  // Print READY once, 200ms after setup completes
  if (!readyPrinted && millis() - setupTime >= 200) {
    if (UART_CON) {
      Serial2.println("UART CONNECTED");
    } else {
      Serial.println("READY");
    }
    readyPrinted = true;
    serialBuffer = ""; // descartar basura del bootloader
  }

  while (dataAvailable()) {
    char c = UART_CON ? (char)Serial2.read() : (char)Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0)
        handleJsonCommand(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r')
      serialBuffer += c;
  }
}

// 3-phase non-blocking stepper driver
// Step time = stepDelayUs*2 us/step. Range: 200–1000 us.
void tickStepper(StepperState &st, StepperPins &pins) {
  if (!st.active)
    return;
  unsigned long now = micros();

  switch (st.phase) {
  case PHASE_DIR_SETUP:
    digitalWrite(pins.dir, st.clockwise ? HIGH : LOW);
    st.phaseStartUs = now;
    st.phase = PHASE_STEP_HIGH;
    break;

  case PHASE_STEP_HIGH:
    if (now - st.phaseStartUs >= DIR_SETUP_DELAY) {
      digitalWrite(pins.step, HIGH);
      st.phaseStartUs = now;
      st.phase = PHASE_STEP_LOW;
    }
    break;

  case PHASE_STEP_LOW:
    if (now - st.phaseStartUs >= st.stepDelayUs) {
      digitalWrite(pins.step, LOW);
      st.position += st.clockwise ? 1 : -1;
      st.stepsRemaining--;

      if (st.stepsRemaining <= 0) {
        st.active = false;
        JsonDocument r;
        r["status"] = 200;
        r["event"] = "moveDone";
        r["motor"] = (pins.step == STEP_PAN_PIN) ? "pan" : "tilt";
        r["position"] = st.position;
        String s;
        serializeJson(r, s);
        sendResponse(s);
      } else {
        // Shift phaseStartUs so PHASE_STEP_HIGH waits stepDelayUs, not
        // DIR_SETUP_DELAY
        st.phaseStartUs = now - DIR_SETUP_DELAY + st.stepDelayUs;
        st.phase = PHASE_STEP_HIGH;
      }
    }
    break;
  }
}

void tickWifi() {
  if (wifiConnState != WIFI_CONNECTING)
    return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnState = WIFI_CONNECTED;
    JsonDocument r;
    r["status"] = 200;
    r["command"] = "connectToWifi";
    r["result"] = "connected";
    r["rssi"] = WiFi.RSSI();
    r["channel"] = WiFi.channel();
    r["frequency"] = 2407 + (5 * WiFi.channel());
    r["txPower"] = WiFi.getTxPower();
    String s;
    serializeJson(r, s);
    sendResponse(s);
    return;
  }

  if (millis() - wifiConnStart >= WIFI_CONN_TIMEOUT_MS) {
    wifiConnState = WIFI_FAILED;
    shutdownWifi();
    JsonDocument r;
    r["status"] = 400;
    r["command"] = "connectToWifi";
    r["result"] = "failed";
    r["error"] = "connection timeout";
    String s;
    serializeJson(r, s);
    sendResponse(s);
  }
}

// RPM = (pulses/s / 2) * 60  — recalculated every second
void tickFanRpm() {
  if (millis() - lastRpmTime >= 1000) {
    noInterrupts();
    unsigned long p = pulses;
    pulses = 0;
    interrupts();
    currentRpm = (p / 2.0) * 60.0;
    lastRpmTime = millis();
  }
}

// ── Info getters ─────────────────────────────────────────────────────────────
void getStepperInfo(JsonDocument &r) {
  r["status"] = 200;
  r["command"] = "getStepperInfo";
  r["panPosition"] = panStepper.position;
  r["tiltPosition"] = tiltStepper.position;
  r["panMoving"] = panStepper.active;
  r["tiltMoving"] = tiltStepper.active;
  r["stepsPerRev"] = STEPS_PER_REV;
  r["panStepDelay"] = panStepper.stepDelayUs;
  r["tiltStepDelay"] = tiltStepper.stepDelayUs;
}

void getFanInfo(JsonDocument &r) {
  r["status"] = 200;
  r["command"] = "getFanInfo";
  r["currentPwmValue"] = currentPwmValue;
  r["currentFanRpm"] = currentRpm;
}

// ── Command handlers ─────────────────────────────────────────────────────────
// Handlers validate params, update state, return immediately.
// Async commands send an ACK now and an event when the operation finishes.
// If a stepper is already moving, a "moveCancelled" event is sent first.

void cmdStepPan(JsonDocument &doc) {
  int steps = doc["stepPanSteps"] | 0;
  if (steps <= 0) {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "stepPanSteps must be greater than 0";
    String s;
    serializeJson(e, s);
    sendResponse(s);
    return;
  }
  if (panStepper.active) {
    JsonDocument c;
    c["status"] = 200;
    c["event"] = "moveCancelled";
    c["motor"] = "pan";
    c["positionAtCancel"] = panStepper.position;
    String s;
    serializeJson(c, s);
    sendResponse(s);
  }
  panStepper.stepsRemaining = steps;
  panStepper.clockwise = doc["stepPanClockwise"] | false;
  panStepper.stepDelayUs =
      constrain((unsigned int)(doc["stepPanStepDelayus"] | 200u), 200u, 1000u);
  panStepper.phase = PHASE_DIR_SETUP;
  panStepper.active = true;

  JsonDocument r;
  r["status"] = 200;
  r["command"] = "stepPan";
  r["steps"] = steps;
  r["clockwise"] = panStepper.clockwise;
  r["stepDelayUs"] = panStepper.stepDelayUs;
  String s;
  serializeJson(r, s);
  sendResponse(s);
}

void cmdStepTilt(JsonDocument &doc) {
  int steps = doc["stepTiltSteps"] | 0;
  if (steps <= 0) {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "stepTiltSteps must be greater than 0";
    String s;
    serializeJson(e, s);
    sendResponse(s);
    return;
  }
  if (tiltStepper.active) {
    JsonDocument c;
    c["status"] = 200;
    c["event"] = "moveCancelled";
    c["motor"] = "tilt";
    c["positionAtCancel"] = tiltStepper.position;
    String s;
    serializeJson(c, s);
    sendResponse(s);
  }
  tiltStepper.stepsRemaining = steps;
  tiltStepper.clockwise = doc["stepTiltClockwise"] | false;
  tiltStepper.stepDelayUs =
      constrain((unsigned int)(doc["stepTiltStepDelayus"] | 200u), 200u, 1000u);
  tiltStepper.phase = PHASE_DIR_SETUP;
  tiltStepper.active = true;

  JsonDocument r;
  r["status"] = 200;
  r["command"] = "stepTilt";
  r["steps"] = steps;
  r["clockwise"] = tiltStepper.clockwise;
  r["stepDelayUs"] = tiltStepper.stepDelayUs;
  String s;
  serializeJson(r, s);
  sendResponse(s);
}

void cmdMovePanTilt(JsonDocument &doc) {
  int pSteps = doc["mptPanSteps"] | 0;
  int tSteps = doc["mptTiltSteps"] | 0;
  if (pSteps <= 0 || tSteps <= 0) {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "mptPanSteps and mptTiltSteps must be greater than 0";
    String s;
    serializeJson(e, s);
    sendResponse(s);
    return;
  }
  unsigned int dly =
      constrain((unsigned int)(doc["mptStepDelayus"] | 200u), 200u, 1000u);

  if (panStepper.active) {
    JsonDocument c;
    c["status"] = 200;
    c["event"] = "moveCancelled";
    c["motor"] = "pan";
    c["positionAtCancel"] = panStepper.position;
    String s;
    serializeJson(c, s);
    sendResponse(s);
  }
  if (tiltStepper.active) {
    JsonDocument c;
    c["status"] = 200;
    c["event"] = "moveCancelled";
    c["motor"] = "tilt";
    c["positionAtCancel"] = tiltStepper.position;
    String s;
    serializeJson(c, s);
    sendResponse(s);
  }

  panStepper.stepsRemaining = pSteps;
  panStepper.clockwise = doc["mptPanCW"] | false;
  panStepper.stepDelayUs = dly;
  panStepper.phase = PHASE_DIR_SETUP;
  panStepper.active = true;
  tiltStepper.stepsRemaining = tSteps;
  tiltStepper.clockwise = doc["mptTiltCW"] | false;
  tiltStepper.stepDelayUs = dly;
  tiltStepper.phase = PHASE_DIR_SETUP;
  tiltStepper.active = true;

  JsonDocument r;
  r["status"] = 200;
  r["command"] = "movePanTilt";
  r["panSteps"] = pSteps;
  r["tiltSteps"] = tSteps;
  r["stepDelayUs"] = dly;
  String s;
  serializeJson(r, s);
  sendResponse(s);
}

void cmdStopMotors() {
  bool wp = panStepper.active, wt = tiltStepper.active;
  panStepper.active = tiltStepper.active = false;
  JsonDocument r;
  r["status"] = 200;
  r["command"] = "stopMotors";
  r["panPosition"] = panStepper.position;
  r["tiltPosition"] = tiltStepper.position;
  r["panWasMoving"] = wp;
  r["tiltWasMoving"] = wt;
  String s;
  serializeJson(r, s);
  sendResponse(s);
}

void cmdConnectWifi(const char *ssid, const char *pass) {
  if (wifiConnState == WIFI_CONNECTING) {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "wifi connection already in progress";
    String s;
    serializeJson(e, s);
    sendResponse(s);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  wifiConnState = WIFI_CONNECTING;
  wifiConnStart = millis();
  JsonDocument r;
  r["status"] = 200;
  r["command"] = "connectToWifi";
  r["result"] = "connecting";
  r["note"] = "async — wait for connected/failed event";
  String s;
  serializeJson(r, s);
  sendResponse(s);
}

// ── Central JSON command dispatcher ─────────────────────────────────────────
void handleJsonCommand(const String &jsonString) {
  // Debug: print raw received JSON line for troubleshooting
  if (UART_CON) {
    Serial2.print("RX: ");
    Serial2.println(jsonString);
  } else {
    Serial.print("RX: ");
    Serial.println(jsonString);
  }
  JsonDocument doc;
  if (deserializeJson(doc, jsonString) != DeserializationError::Ok) {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "JSON parse error";
    String s;
    serializeJson(e, s);
    sendResponse(s);
    return;
  }

  if (doc["stepPanSteps"].is<int>())
    cmdStepPan(doc);
  else if (doc.containsKey("stepTiltSteps"))
    cmdStepTilt(doc);
  else if (doc.containsKey("mptPanSteps"))
    cmdMovePanTilt(doc);
  else if (doc.containsKey("stopMotors"))
    cmdStopMotors();
  else if (doc.containsKey("getStepperInfo")) {
    JsonDocument r;
    getStepperInfo(r);
    String s;
    serializeJson(r, s);
    sendResponse(s);
  } else if (doc.containsKey("pwmValue")) {
    int pwm = doc["pwmValue"] | -1;
    if (pwm < 0 || pwm > 255) {
      JsonDocument e;
      e["status"] = 400;
      e["error"] = "pwmValue must be 0-255";
      String s;
      serializeJson(e, s);
      sendResponse(s);
    } else {
      currentPwmValue = pwm;
      ledcWrite(0, pwm);
      JsonDocument r;
      r["status"] = 200;
      r["command"] = "setFanPWM";
      r["pwmValue"] = pwm;
      String s;
      serializeJson(r, s);
      sendResponse(s);
    }
  } else if (doc.containsKey("getFanRpm")) {
    JsonDocument r;
    r["status"] = 200;
    r["command"] = "getFanRpm";
    r["fanRpm"] = currentRpm;
    String s;
    serializeJson(r, s);
    sendResponse(s);
  } else if (doc.containsKey("getFanInfo")) {
    JsonDocument r;
    getFanInfo(r);
    String s;
    serializeJson(r, s);
    sendResponse(s);
  } else if (doc.containsKey("connectToWifi")) {
    const char *ssid = doc["ssid"] | "", *pass = doc["password"] | "";
    if (ssid[0] == '\0' || pass[0] == '\0') {
      JsonDocument e;
      e["status"] = 400;
      e["error"] = "ssid and password required";
      String s;
      serializeJson(e, s);
      sendResponse(s);
      return;
    }
    cmdConnectWifi(ssid, pass);
  } else if (doc.containsKey("getWifiInfo")) {
    JsonDocument r;
    r["status"] = 200;
    r["command"] = "getWifiInfo";
    r["connected"] = (WiFi.status() == WL_CONNECTED);
    r["connecting"] = (wifiConnState == WIFI_CONNECTING);
    if (WiFi.status() == WL_CONNECTED) {
      r["rssi"] = WiFi.RSSI();
      r["channel"] = WiFi.channel();
      r["frequency"] = 2407 + (5 * WiFi.channel());
      r["txPower"] = WiFi.getTxPower();
    }
    String s;
    serializeJson(r, s);
    sendResponse(s);
  } else if (doc.containsKey("disconnectWifi")) {
    shutdownWifi();
    JsonDocument r;
    r["status"] = 200;
    r["command"] = "disconnectWifi";
    r["result"] = "disconnected";
    String s;
    serializeJson(r, s);
    sendResponse(s);
  } else {
    JsonDocument e;
    e["status"] = 400;
    e["error"] = "unknown command";
    String s;
    serializeJson(e, s);
    sendResponse(s);
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(50);
  while (Serial.available())
    Serial.read(); // flush basura del bootloader
  setupTime = millis();

  pinMode(STEP_PAN_PIN, OUTPUT);
  pinMode(DIR_PAN_PIN, OUTPUT);
  pinMode(STEP_TILT_PIN, OUTPUT);
  pinMode(DIR_TILT_PIN, OUTPUT);
  pinMode(FAN_TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), countPulse, FALLING);

  ledcSetup(0, 25000, 8);
  ledcAttachPin(FAN_PWM_PIN, 0);
  ledcWrite(0, 128);

  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();

  initComm();
}

// ── Loop — cooperative scheduler, zero blocking ──────────────────────────────
void loop() {
  tickSerial();
  tickStepper(panStepper, panPins);
  tickStepper(tiltStepper, tiltPins);
  tickWifi();
  tickFanRpm();
}