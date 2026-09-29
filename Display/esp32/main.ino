#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include <esp_timer.h>
#include <math.h>

/* ==========================================================
   CONFIG
   ========================================================== */

constexpr float TYRE_DIAMETER_M = 0.4064f; 
constexpr float TYRE_CIRCUMFERENCE_M = TYRE_DIAMETER_M * 3.14159265f;

constexpr float RPM_PULSES_PER_REV   = 1.0f; //A temporary value to be changed during testing
constexpr float SPEED_PULSES_PER_REV = 1.0f; //A temporary value to be changed during testing

constexpr float ADC_VREF = 5.0f;

constexpr float R_PULLUP = 10000.0f;
constexpr float R25      = 10000.0f;
constexpr float BETA     = 3977.0f;
constexpr float T25_K    = 298.15f;

constexpr uint16_t LOOP_HZ = 30;

constexpr uint32_t RPM_TIMEOUT_US   = 500000;
constexpr uint32_t SPEED_TIMEOUT_US = 1000000;
constexpr uint32_t GPS_TIMEOUT_US   = 2000000;

/* sensor polarity */
constexpr bool OIL_ACTIVE_LOW     = true;
constexpr bool NEUTRAL_ACTIVE_LOW = true;

/* ==========================================================
   PINS
   ========================================================== */

constexpr uint8_t RPM_OUTPUT   = 32;
constexpr uint8_t SPEED_OUTPUT = 33;

constexpr uint8_t OIL_PRESSURE_PIN = 26;
constexpr uint8_t NEUTRAL_PIN      = 27;

/* MCP3008 */
constexpr uint8_t ADC_CS   = 5;
constexpr uint8_t ADC_CLK  = 18;
constexpr uint8_t ADC_DOUT = 19;
constexpr uint8_t ADC_DIN  = 23;

/* GPS */
HardwareSerial GPS(1);
constexpr uint8_t GPS_RX = 16;
constexpr uint8_t GPS_TX = 17;

/* ==========================================================
   GLOBALS
   ========================================================== */

StaticJsonDocument<512> telemetryJson;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/* ISR DATA */
volatile uint64_t lastRpmUs    = 0;
volatile uint64_t rpmPeriodUs  = 0;

volatile uint64_t lastSpeedUs   = 0;
volatile uint64_t speedPeriodUs = 0;

/* GPS */
char gpsBuffer[96] = "";
volatile uint64_t lastGpsFixUs = 0;
volatile bool gpsFixActive = false;

/* ==========================================================
   MCP3008
   ========================================================== */

void initMCP3008() {
    pinMode(ADC_CS, OUTPUT);
    digitalWrite(ADC_CS, HIGH);

    SPI.begin(ADC_CLK, ADC_DOUT, ADC_DIN, ADC_CS);
}

uint16_t readADC(uint8_t channel) {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(ADC_CS, LOW);

    SPI.transfer(0x01);
    uint8_t high = SPI.transfer((0x08 | channel) << 4);
    uint8_t low  = SPI.transfer(0x00);

    digitalWrite(ADC_CS, HIGH);
    SPI.endTransaction();

    return ((high & 0x03) << 8) | low;
}

/* ==========================================================
   INTERRUPTS
   ========================================================== */

constexpr uint32_t MIN_RPM_DEBOUNCE_US   = 250;
constexpr uint32_t MIN_SPEED_DEBOUNCE_US = 500;

void IRAM_ATTR rpmISR() {
    uint64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&mux);

    uint64_t diff = now - lastRpmUs;
    if (diff > MIN_RPM_DEBOUNCE_US) {
        rpmPeriodUs = diff;
        lastRpmUs = now;
    }

    portEXIT_CRITICAL_ISR(&mux);
}

void IRAM_ATTR speedISR() {
    uint64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&mux);

    uint64_t diff = now - lastSpeedUs;
    if (diff > MIN_SPEED_DEBOUNCE_US) {
        speedPeriodUs = diff;
        lastSpeedUs = now;
    }

    portEXIT_CRITICAL_ISR(&mux);
}

/* ==========================================================
   SAFE READS
   ========================================================== */

uint64_t getRpmPeriod() {
    uint64_t p, t;

    portENTER_CRITICAL(&mux);
    p = rpmPeriodUs;
    t = lastRpmUs;
    portEXIT_CRITICAL(&mux);

    if (!p) return 0;
    if (esp_timer_get_time() - t > RPM_TIMEOUT_US) return 0;
    if (p < 50) return 0;

    return p;
}

uint64_t getSpeedPeriod() {
    uint64_t p, t;

    portENTER_CRITICAL(&mux);
    p = speedPeriodUs;
    t = lastSpeedUs;
    portEXIT_CRITICAL(&mux);

    if (!p) return 0;
    if (esp_timer_get_time() - t > SPEED_TIMEOUT_US) return 0;
    if (p < 50) return 0;

    return p;
}

/* ==========================================================
   CALCULATIONS
   ========================================================== */

float getRPM() {
    uint64_t period = getRpmPeriod();
    if (!period) return 0.0f;

    return 60e6f / (period * RPM_PULSES_PER_REV);
}

float getSpeedKph() {
    uint64_t period = getSpeedPeriod();
    if (!period) return 0.0f;

    float revPerSec = (1e6f / period) / SPEED_PULSES_PER_REV;
    return revPerSec * TYRE_CIRCUMFERENCE_M * 3.6f;
}

float getCoolantTempC() {
    uint16_t adc = readADC(0);

    if (adc == 0) return -40.0f;

    float vin = (adc / 1023.0f) * ADC_VREF;

    float denom = ADC_VREF - vin;
    if (denom <= 0.0001f) return -40.0f;

    float r = (R_PULLUP * vin) / denom;

    float invT = (1.0f / T25_K) +
                 (log(r / R25) / BETA);

    return (1.0f / invT) - 273.15f;
}

/* ==========================================================
   GPS
   ========================================================== */

void readGPS() {
    static char line[120];
    static uint8_t idx = 0;

    uint64_t now = esp_timer_get_time();

    while (GPS.available()) {
        char c = GPS.read();

        if (c == '\n') {
            line[idx] = '\0';
            idx = 0;

            if (strncmp(line, "$GPRMC", 6) && strncmp(line, "$GNRMC", 6))
                continue;

            char *asterisk = strchr(line, '*');
            if (asterisk) *asterisk = '\0';

            char *field[12] = {nullptr};
            uint8_t i = 0;

            char *tok = strtok(line, ",");
            while (tok && i < 12) {
                field[i++] = tok;
                tok = strtok(nullptr, ",");
            }

            if (i < 7 || !field[2]) continue;

            gpsFixActive = (field[2][0] == 'A');

            if (!gpsFixActive) continue;

            snprintf(gpsBuffer, sizeof(gpsBuffer),
                     "%s,%s,%s,%s",
                     field[3] ? field[3] : "",
                     field[4] ? field[4] : "",
                     field[5] ? field[5] : "",
                     field[6] ? field[6] : "");

            lastGpsFixUs = now;
        }
        else if (idx < sizeof(line) - 1) {
            line[idx++] = c;
        }
    }
}

/* ==========================================================
   SETUP
   ========================================================== */

void setup() {
    Serial.begin(115200);

    pinMode(RPM_OUTPUT, INPUT_PULLUP);
    pinMode(SPEED_OUTPUT, INPUT_PULLUP);
    pinMode(OIL_PRESSURE_PIN, INPUT_PULLUP);
    pinMode(NEUTRAL_PIN, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(RPM_OUTPUT), rpmISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(SPEED_OUTPUT), speedISR, FALLING);

    GPS.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

    initMCP3008();
}

/* ==========================================================
   LOOP
   ========================================================== */

void loop() {
    static uint32_t lastTick = 0;

    readGPS();

    if (millis() - lastTick < (1000 / LOOP_HZ))
        return;

    lastTick = millis();

    bool gpsValid =
        gpsFixActive &&
        (esp_timer_get_time() - lastGpsFixUs < GPS_TIMEOUT_US);

    telemetryJson.clear();

    telemetryJson["RPM"]   = (int)round(getRPM());
    telemetryJson["speed"] = (int)round(getSpeedKph());

    telemetryJson["coolantTemp"] = (int)round(getCoolantTempC());

    telemetryJson["GPS"] = gpsValid ? gpsBuffer : nullptr;

    telemetryJson["oilPressureWarning"] =
        OIL_ACTIVE_LOW ? (digitalRead(OIL_PRESSURE_PIN) == LOW)
                       : (digitalRead(OIL_PRESSURE_PIN) == HIGH);

    telemetryJson["Neutral"] =
        NEUTRAL_ACTIVE_LOW ? (digitalRead(NEUTRAL_PIN) == LOW)
                           : (digitalRead(NEUTRAL_PIN) == HIGH);

    serializeJson(telemetryJson, Serial);
    Serial.println();
}
