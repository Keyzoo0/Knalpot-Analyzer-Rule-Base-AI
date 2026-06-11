/*
  ESP32 Knalpot Combustion Quality Analyzer
  - MQ-2 (HC ppm)  -> GPIO 35
  - MQ-7 (CO %)    -> GPIO 34
  - Buzzer         -> GPIO 13
  - Motor (relay)  -> GPIO 14 (active HIGH)
  - TFT ILI9341    -> SCK 18, MISO 19, MOSI 23, CS 5, DC 2, RST 4
  - Touch XPT2046  -> share SPI bus, T_CS 15 (T_IRQ tidak dipakai)
  Libraries (install via Arduino Library Manager):
    WiFi, Preferences, LittleFS, SPI, ESPmDNS
    Adafruit_GFX, Adafruit_ILI9341
    XPT2046_Touchscreen (by Paul Stoffregen)
    ESPAsyncWebServer, AsyncTCP, ArduinoJson
*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <esp_system.h>   // esp_reset_reason() untuk diagnosa penyebab restart

// ---------------- PIN ----------------
#define PIN_BUZZER   13
#define PIN_MOTOR    14
#define PIN_MQ7      34   // CO %
#define PIN_MQ2      35   // HC ppm
#define TFT_MOSI     19
#define TFT_MISO     23
#define TFT_SCK      18
#define TFT_CS        5
#define TFT_DC        2
#define TFT_RST       4
#define TOUCH_CS     15
#define SD_CS        26   // SD card di modul TFT, share SCK/MISO/MOSI dengan TFT/touch

// Touch calibration (sesuaikan jika perlu — angka dari kode referensi)
#define TOUCH_XMIN   675
#define TOUCH_XMAX   3502
#define TOUCH_YMIN   843
#define TOUCH_YMAX   3207
#define TOUCH_PRESS_MIN 200    // tekanan minimum (z) — jangan terlalu tinggi (susah tekan)
#define TOUCH_DEBOUNCE   60    // jeda minimum antar tap (ms) — responsif tapi anti-bounce
#define TOUCH_RELEASE_MS 20    // berapa lama harus "lepas" sebelum tap berikutnya valid

// Clock SPI untuk TFT. TFT & touch berbagi bus; pada clock tinggi sinyal TFT
// marjinal -> layar kadang putih/korup (bug terkenal modul ILI9341+XPT2046).
// 10MHz konservatif & stabil untuk modul murah. Naikkan bila ingin redraw lebih cepat.
#define TFT_SPI_HZ      10000000

// Periode polling touch (ms). 15ms = 66Hz — responsif, masih cukup jeda untuk
// tidak rapat dengan transaksi SPI TFT.
#define TOUCH_POLL_MS   15

// ---------------- WIFI ----------------
const char* WIFI_SSID = "Biznet";
const char* WIFI_PASS = "12345678";

// ---------------- ADC / Sensor constants ----------------
// === Pembagi tegangan untuk turunkan Vout MQ (5V) ke level ADC ESP32 (3.3V) ===
//   Vout MQ ── R1 ──┬── ADC ESP32
//                   R2
//                   GND
// Vadc = Vmodule * R2 / (R1 + R2)  =>  Vmodule = Vadc * (R1 + R2) / R2
#define DIV_R1          4700.0f      // 4.7k ohm (atas)
#define DIV_R2          10000.0f     // 10k  ohm (bawah, ke GND)

#define VCC_MODULE      5.0f         // tegangan suplai modul MQ
#define RL_MODULE       10000.0f     // load resistor di PCB modul (umumnya 10k)
#define ADC_VREF        3.3f
#define ADC_MAX         4095
#define MQ_SAMPLES      16           // jumlah sampel rata-rata per pembacaan

// MQ-2 LPG/HC curve (Rs/R0 -> ppm), approx datasheet
#define MQ2_A           574.25f
#define MQ2_B           -2.222f
// MQ-7 CO curve
#define MQ7_A           99.042f
#define MQ7_B           -1.518f

// Target kalibrasi idle default (bisa diubah dari web): HC=75 ppm, CO=0.29% (=2900 ppm)
#define CALIB_TARGET_HC_PPM   75.0f
#define CALIB_TARGET_CO_PPM   2900.0f
#define CALIB_SAMPLES         50      // 50 sampel x ~200ms ≈ 10 detik

// ---------------- Globals ----------------
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

// Runtime settings (loaded from NVS)
struct IndexEntry {
  String label;
  float th_hc;
  float th_co;
};
struct Settings {
  uint32_t inhale_ms = 10000;
  uint32_t preprocess_ms = 5000;
  uint32_t sample_interval_ms = 1000;
  uint16_t sample_count = 10;
  float r0_mq2 = 10000.0f;
  float r0_mq7 = 10000.0f;
  std::vector<IndexEntry> indices;
} cfg;

enum SysState {
  ST_IDLE,
  ST_SELECT_IDX,
  ST_WAIT_VALVE,
  ST_INHALE,
  ST_PREPROCESS,
  ST_SAMPLING,
  ST_RESULT
};
SysState state = ST_IDLE;

// Struct deklarasi di sini (sebelum fungsi pertama) supaya auto-prototype Arduino
// bisa melihatnya. Tanpa ini, fungsi seperti `bool tapped(TBtn,..)` gagal di-prototyping.
struct TBtn { int x, y, w, h; };

String stateName(SysState s) {
  switch(s){
    case ST_IDLE: return "IDLE";
    case ST_SELECT_IDX: return "SELECT_IDX";
    case ST_WAIT_VALVE: return "WAIT_VALVE";
    case ST_INHALE: return "INHALE";
    case ST_PREPROCESS: return "PREPROCESS";
    case ST_SAMPLING: return "SAMPLING";
    case ST_RESULT: return "RESULT";
  }
  return "?";
}

// ---- TFT chart buffer (untuk grafik di layar) ----
#define CHART_N 80
float chartHC[CHART_N];
float chartCO[CHART_N];
int   chartHead = 0;
int   chartCount = 0;

// ---- Touch + screen state ----
int    idxScrollOffset = 0;   // untuk halaman SELECT_IDX

// Forward declarations untuk dipakai oleh enterState() yang ada di atas TFT section
void chartReset();
void chartPush(float hc, float co);
void broadcastState();
void saveLogEntry();
String currentTimestamp();

uint32_t phaseStart = 0;
uint32_t phaseDuration = 0;
int   selectedIndex = -1;
bool  motorOn = false;
bool  flushManual = false;

// Sampling buffers
#define MAX_SAMPLES 200
float sumHC = 0, sumCO = 0;
uint16_t sampledCount = 0;
uint32_t lastSampleAt = 0;
float    sampleHC[MAX_SAMPLES];   // nilai per-sample saat SAMPLING (untuk log)
float    sampleCO[MAX_SAMPLES];

// SD card
bool sd_ok = false;
bool lastLogSaved = false;              // status simpan log terakhir (dikirim di frame result)
const char* LOG_PATH = "/logs.jsonl";   // JSON-lines: 1 baris = 1 log entry
const char* LOG_TMP  = "/logs.tmp";     // file sementara saat rewrite (edit entry)

// Latest readings
float lastHC = 0, lastCO = 0;
int   lastAdcMQ2 = 0, lastAdcMQ7 = 0;

// Result
float resAvgHC = 0, resAvgCO = 0;
String resultLabel = "";
uint8_t resultCode = 0; // 0 normal,1 gejala,2 misfire,3 tidak normal

// Sensor health
bool mq2_ok = true, mq7_ok = true, tft_ok = false;

// Buzzer non-blocking
uint32_t buzzerOffAt = 0;
bool buzzerActive = false;
uint8_t buzzerPattern = 0; // 0=off,1=run,2=transition,3=finish
uint32_t buzzerPhaseStart = 0;
uint8_t  buzzerStep = 0;

// TFT redraw
uint32_t lastTftRefresh = 0;
SysState lastTftState = ST_IDLE;
// Minta full-redraw layar saat ini pada tftRefresh() berikutnya. Touch handler
// HANYA boleh set flag ini, JANGAN menggambar TFT langsung — supaya transaksi SPI
// touch (ts.getPoint) tidak bertabrakan dengan transaksi SPI TFT (white screen).
volatile bool tftDirty = false;

// ---------------- Helpers ----------------
void setMotor(bool on) {
  motorOn = on;
  digitalWrite(PIN_MOTOR, on ? HIGH : LOW);
}

void buzzerOn(uint16_t ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerActive = true;
  buzzerOffAt = millis() + ms;
}
void buzzerOff() {
  digitalWrite(PIN_BUZZER, LOW);
  buzzerActive = false;
}
void setBuzzerPattern(uint8_t p) {
  buzzerPattern = p;
  buzzerPhaseStart = millis();
  buzzerStep = 0;
  buzzerOff();
}
void handleBuzzer() {
  uint32_t now = millis();
  if (buzzerActive && now >= buzzerOffAt) buzzerOff();
  if (buzzerPattern == 0) return;
  uint32_t elapsed = now - buzzerPhaseStart;
  if (buzzerPattern == 1) {
    // 2 beeps per second: beep 80ms at t=0, beep 80ms at t=500
    uint32_t cyc = elapsed % 1000;
    if (cyc < 80 && !buzzerActive && buzzerStep == 0) { buzzerOn(80); buzzerStep = 1; }
    else if (cyc >= 80 && cyc < 500) buzzerStep = 2;
    else if (cyc >= 500 && cyc < 580 && !buzzerActive && buzzerStep == 2) { buzzerOn(80); buzzerStep = 3; }
    else if (cyc >= 580) buzzerStep = 0;
  } else if (buzzerPattern == 2) {
    // 3 quick beeps then idle (one-shot)
    uint32_t cyc = elapsed;
    if (buzzerStep == 0 && cyc < 60) { buzzerOn(60); buzzerStep = 1; }
    else if (buzzerStep == 1 && cyc >= 150 && cyc < 210) { buzzerOn(60); buzzerStep = 2; }
    else if (buzzerStep == 2 && cyc >= 300 && cyc < 360) { buzzerOn(60); buzzerStep = 3; }
    else if (buzzerStep == 3 && cyc >= 500) { buzzerPattern = 0; }
  } else if (buzzerPattern == 3) {
    // one long beep
    if (buzzerStep == 0) { buzzerOn(1000); buzzerStep = 1; }
    else if (buzzerStep == 1 && !buzzerActive) buzzerPattern = 0;
  }
}

// ---------------- Sensor reading (MQ-2 & MQ-7) ----------------

// Filter Kalman 1D (scalar) untuk menghaluskan pembacaan ADC MQ.
// q = process noise (semakin besar = respon cepat, smoothing kurang)
// r = measurement noise (semakin besar = smoothing kuat, respon lambat)
// Dengan polling 200ms, q=1/r=30 memberi lag efektif ~5 sampel (~1 detik).
#define KF_Q  1.0f
#define KF_R  30.0f

struct Kalman1D {
  float q, r;        // kovarians noise proses & pengukuran
  float p = 1.0f;    // kovarians estimasi
  float x = 0.0f;    // estimasi state
  bool  init = false;
  float update(float z) {
    if (!init) { x = z; init = true; return x; }   // seed dengan sampel pertama
    p += q;
    float k = p / (p + r);   // Kalman gain
    x += k * (z - x);
    p *= (1.0f - k);
    return x;
  }
};

Kalman1D kfMQ2{KF_Q, KF_R};
Kalman1D kfMQ7{KF_Q, KF_R};

// 1) Baca ADC mentah, sudah di-rata-rata
int readAdcAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < MQ_SAMPLES; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (int)(sum / MQ_SAMPLES);
}

// 2) ADC mentah -> tegangan di kaki ADC (0..3.3V)
//    Param float agar bisa menerima nilai ADC hasil filter Kalman.
float adcToVadc(float adc) {
  return (adc / (float)ADC_MAX) * ADC_VREF;
}

// 3) Tegangan ADC -> tegangan asli Vout modul MQ (sebelum pembagi tegangan)
//    Vmodule = Vadc * (R1 + R2) / R2
float vadcToVmodule(float vadc) {
  return vadc * (DIV_R1 + DIV_R2) / DIV_R2;
}

// 4) Vmodule -> resistansi sensor Rs
//    Modul MQ: Vout = Vcc * RL / (RL + Rs)  =>  Rs = RL * (Vcc - Vout) / Vout
float vmoduleToRs(float vmodule) {
  if (vmodule < 0.01f) return 1e9f; // hindari pembagian nol
  return RL_MODULE * (VCC_MODULE - vmodule) / vmodule;
}

// 5) Rs/R0 -> ppm berdasarkan kurva datasheet ppm = a * (Rs/R0)^b
float ratioToPpm(float rs, float r0, float a, float b) {
  if (r0 < 1.0f) return 0;
  float ratio = rs / r0;
  if (ratio <= 0) return 0;
  return a * pow(ratio, b);
}

// Baca MQ-2 -> HC ppm
// ADC mentah tetap disimpan di lastAdcMQ2 untuk health check (deteksi rail 4095);
// konversi ppm memakai nilai ADC yang sudah dihaluskan filter Kalman.
float readMQ2_HC() {
  lastAdcMQ2   = readAdcAvg(PIN_MQ2);
  float adcF   = kfMQ2.update((float)lastAdcMQ2);
  float vadc   = adcToVadc(adcF);
  float vmod   = vadcToVmodule(vadc);
  float rs     = vmoduleToRs(vmod);
  return ratioToPpm(rs, cfg.r0_mq2, MQ2_A, MQ2_B);
}

// Baca MQ-7 -> CO % (datasheet pakai ppm; 1% volume = 10000 ppm)
float readMQ7_CO() {
  lastAdcMQ7   = readAdcAvg(PIN_MQ7);
  float adcF   = kfMQ7.update((float)lastAdcMQ7);
  float vadc   = adcToVadc(adcF);
  float vmod   = vadcToVmodule(vadc);
  float rs     = vmoduleToRs(vmod);
  float ppm    = ratioToPpm(rs, cfg.r0_mq7, MQ7_A, MQ7_B);
  return ppm / 10000.0f;   // ppm -> %
}

void readSensors() {
  lastHC = readMQ2_HC();
  lastCO = readMQ7_CO();
  // Health check:
  //   Dengan voltage divider (R2 pull ke GND), sensor terputus DAN sensor dengan
  //   output 0V sama-sama membaca ADC ~0. Jadi nilai rendah (0V) TIDAK bisa
  //   dipakai sebagai indikasi disconnect -> 0V dianggap VALID (idle normal).
  //   Yang ditandai fault hanya jika ADC mentok di rail paling atas (4095),
  //   yang menandakan pin short ke 3.3V / divider lepas / kabel salah.
  mq2_ok = (lastAdcMQ2 < 4095);
  mq7_ok = (lastAdcMQ7 < 4095);
}

// ---------------- Kalibrasi target idle (non-blocking) ----------------
// PENTING: kalibrasi TIDAK boleh blocking di handler HTTP — handler jalan di task
// async_tcp yang diawasi task watchdog (timeout 5 detik). Blocking 10 detik di sana
// memicu "task_wdt: async_tcp" -> abort -> restart.
// Solusi: endpoint hanya memicu start; sampling dicicil di loop() via calibTick();
// hasil dikirim ke web lewat WebSocket (type=calib_done) + bisa dipoll via GET.
// Rumus: R0 = Rs * (target_ppm / a)^(-1/b)
bool     calibActive     = false;
uint8_t  calibCount      = 0;
float    calibRs2Sum     = 0, calibRs7Sum = 0;
float    calibTargetHc   = CALIB_TARGET_HC_PPM;  // ppm
float    calibTargetCo   = CALIB_TARGET_CO_PPM;  // ppm (% x 10000)
uint32_t calibLastSample = 0;
String   calibResultJson = "";                   // hasil terakhir (fallback poll)

void calibStart(float targetHcPpm, float targetCoPpm) {
  calibTargetHc   = targetHcPpm;
  calibTargetCo   = targetCoPpm;
  calibCount      = 0;
  calibRs2Sum     = 0;
  calibRs7Sum     = 0;
  calibLastSample = 0;
  calibActive     = true;
  Serial.printf("[CALIB] start target HC=%.0f ppm CO=%.2f %%\n",
                calibTargetHc, calibTargetCo / 10000.0f);
}

void calibFinish(bool ok, const char* err, float r0_2, float r0_7) {
  calibActive = false;
  JsonDocument d;
  d["type"] = "calib_done";
  d["ok"]   = ok;
  if (ok) {
    d["r0_mq2"]        = r0_2;
    d["r0_mq7"]        = r0_7;
    d["target_hc"]     = calibTargetHc;
    d["target_co_pct"] = calibTargetCo / 10000.0f;
  } else {
    d["err"] = err;
  }
  calibResultJson = "";
  serializeJson(d, calibResultJson);
  wsSend(calibResultJson);
  Serial.printf("[CALIB] %s r0_mq2=%.1f r0_mq7=%.1f\n", ok ? "OK" : err, r0_2, r0_7);
}

// Dipanggil dari loop(): ambil 1 sampel tiap 200ms sampai CALIB_SAMPLES.
void calibTick() {
  if (!calibActive) return;
  if (state != ST_IDLE) {           // run dimulai di tengah kalibrasi -> batalkan
    calibFinish(false, "dibatalkan: system run dimulai", 0, 0);
    return;
  }
  uint32_t now = millis();
  if (now - calibLastSample < 200) return;
  calibLastSample = now;
  int a2 = readAdcAvg(PIN_MQ2);
  int a7 = readAdcAvg(PIN_MQ7);
  if (a2 >= 4095 || a7 >= 4095) {
    calibFinish(false, "sensor disconnect / pembacaan invalid", 0, 0);
    return;
  }
  calibRs2Sum += vmoduleToRs(vadcToVmodule(adcToVadc(a2)));
  calibRs7Sum += vmoduleToRs(vadcToVmodule(adcToVadc(a7)));
  if (++calibCount < CALIB_SAMPLES) return;
  // Semua sampel terkumpul -> back-solve R0 dari target
  float rs2  = calibRs2Sum / CALIB_SAMPLES;
  float rs7  = calibRs7Sum / CALIB_SAMPLES;
  float r0_2 = rs2 * pow(calibTargetHc / MQ2_A, -1.0f / MQ2_B);
  float r0_7 = rs7 * pow(calibTargetCo / MQ7_A, -1.0f / MQ7_B);
  bool ok = (r0_2 > 0 && r0_7 > 0 && isfinite(r0_2) && isfinite(r0_7));
  if (ok) {
    cfg.r0_mq2 = r0_2;
    cfg.r0_mq7 = r0_7;
    saveSettings();
  }
  calibFinish(ok, "hasil R0 invalid", r0_2, r0_7);
}

// ---------------- Settings storage ----------------
void saveSettings() {
  prefs.begin("knl", false);
  prefs.putUInt("inhale", cfg.inhale_ms);
  prefs.putUInt("prep", cfg.preprocess_ms);
  prefs.putUInt("sint", cfg.sample_interval_ms);
  prefs.putUShort("scnt", cfg.sample_count);
  prefs.putFloat("r0_2", cfg.r0_mq2);
  prefs.putFloat("r0_7", cfg.r0_mq7);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : cfg.indices) {
    JsonObject o = arr.add<JsonObject>();
    o["label"] = e.label;
    o["th_hc"] = e.th_hc;
    o["th_co"] = e.th_co;
  }
  String s; serializeJson(doc, s);
  prefs.putString("idx", s);
  prefs.end();
}

void loadSettings() {
  prefs.begin("knl", true);
  cfg.inhale_ms = prefs.getUInt("inhale", 10000);
  cfg.preprocess_ms = prefs.getUInt("prep", 5000);
  cfg.sample_interval_ms = prefs.getUInt("sint", 1000);
  cfg.sample_count = prefs.getUShort("scnt", 10);
  cfg.r0_mq2 = prefs.getFloat("r0_2", 10000.0f);
  cfg.r0_mq7 = prefs.getFloat("r0_7", 10000.0f);
  String s = prefs.getString("idx", "");
  prefs.end();
  cfg.indices.clear();
  if (s.length() > 0) {
    JsonDocument doc;
    if (!deserializeJson(doc, s)) {
      for (JsonObject o : doc.as<JsonArray>()) {
        IndexEntry e;
        e.label = o["label"].as<String>();
        e.th_hc = o["th_hc"] | 500.0f;
        e.th_co = o["th_co"] | 2.0f;
        cfg.indices.push_back(e);
      }
    }
  }
  if (cfg.indices.empty()) {
    cfg.indices.push_back({"Motor < 2010", 800, 3.0});
    cfg.indices.push_back({"Motor 2010-2015", 500, 2.0});
    cfg.indices.push_back({"Motor > 2015", 300, 1.0});
    saveSettings();
  }
}

// ---------------- Classification ----------------
void classify() {
  if (sampledCount == 0) { resultLabel = "NO DATA"; resultCode = 255; return; }
  resAvgHC = sumHC / sampledCount;
  resAvgCO = sumCO / sampledCount;
  if (selectedIndex < 0 || selectedIndex >= (int)cfg.indices.size()) {
    resultLabel = "INDEX INVALID"; resultCode = 255; return;
  }
  float thHC = cfg.indices[selectedIndex].th_hc;
  float thCO = cfg.indices[selectedIndex].th_co;
  bool hcHi = resAvgHC >= thHC;
  bool coHi = resAvgCO >= thCO;
  if (!hcHi && !coHi)      { resultLabel = "NORMAL";                       resultCode = 0; }
  else if (!hcHi && coHi)  { resultLabel = "GEJALA AWAL";                  resultCode = 1; }
  else if (hcHi && !coHi)  { resultLabel = "MISFIRE / PEMBAKARAN TAK SEMPURNA"; resultCode = 2; }
  else                     { resultLabel = "PEMBAKARAN TIDAK NORMAL";      resultCode = 3; }
}

// ---------------- WS broadcast ----------------
// Guard: skip jika tidak ada client (hindari alokasi buffer sia-sia & penumpukan
// antrian async yang bisa menghabiskan heap -> hang).
void wsSend(const String& s) {
  if (ws.count() == 0) return;
  ws.textAll(s);
}

void broadcastData() {
  if (ws.count() == 0) return;
  // Skip data frame saat antrian client penuh: data bersifat periodik (akan dikirim
  // ulang 200ms lagi). Membiarkan antrian lega supaya frame KRITIS (state/result)
  // tidak ikut ter-drop & heap tidak menumpuk.
  if (!ws.availableForWriteAll()) return;
  JsonDocument d;
  d["type"]    = "data";
  d["hc"]      = lastHC;
  d["co"]      = lastCO;
  d["state"]   = stateName(state);
  d["motor"]   = motorOn;
  bool inRun = (state==ST_INHALE || state==ST_PREPROCESS || state==ST_SAMPLING);
  d["elapsed"] = inRun ? (millis() - phaseStart) : 0;
  d["phase_total"] = phaseDuration;
  d["sampled"] = sampledCount;
  d["sample_target"] = cfg.sample_count;
  // Field state-snapshot agar web bisa self-heal walau frame "state" sempat ter-drop.
  d["selected_index"] = selectedIndex;
  d["flush"] = flushManual;
  String s; serializeJson(d, s); wsSend(s);
}
void broadcastState() {
  JsonDocument d;
  d["type"]  = "state";
  d["state"] = stateName(state);
  d["phase_total"] = phaseDuration;
  d["selected_index"] = selectedIndex;
  d["motor"] = motorOn;
  d["flush"] = flushManual;
  String s; serializeJson(d, s); wsSend(s);
}
void broadcastResult() {
  JsonDocument d;
  d["type"]   = "result";
  d["label"]  = resultLabel;
  d["code"]   = resultCode;
  d["avg_hc"] = resAvgHC;
  d["avg_co"] = resAvgCO;
  d["th_hc"]  = (selectedIndex>=0)?cfg.indices[selectedIndex].th_hc:0;
  d["th_co"]  = (selectedIndex>=0)?cfg.indices[selectedIndex].th_co:0;
  d["index_label"] = (selectedIndex>=0)?cfg.indices[selectedIndex].label:String("");
  d["log_saved"] = lastLogSaved;   // status simpan SD utk ditampilkan di web
  String s; serializeJson(d, s); wsSend(s);
}

// ---------------- NTP & Logger (SD card) ----------------
String currentTimestamp() {
  struct tm ti;
  if (!getLocalTime(&ti, 50)) {
    // belum sync -> pakai uptime sebagai fallback
    char b[24]; snprintf(b, sizeof(b), "up+%lus", (unsigned long)(millis()/1000));
    return String(b);
  }
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &ti);
  return String(b);
}

// Tulis 1 baris JSONL ke SD; return jumlah byte tertulis (0 = gagal)
static size_t writeLogLine(JsonDocument& d) {
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) return 0;
  size_t n = serializeJson(d, f);
  if (n > 0) n += f.print('\n');
  f.close();
  return n;
}

// Append 1 log entry (JSONL) ke SD.
// SD ada di bus SPI bersama TFT/Touch — kadang card "tersesat" setelah banyak
// transaksi TFT walau init boot OK. Jika tulis gagal: remount SD lalu retry 1x.
void saveLogEntry() {
  lastLogSaved = false;
  if (!sd_ok) { Serial.println("[LOG] SD not ready, skip"); return; }

  JsonDocument d;
  d["ts"]    = currentTimestamp();
  d["up"]    = (uint32_t)(millis() / 1000);
  d["vehicle"] = "";   // nama kendaraan — diisi/diedit dari web (TFT tanpa keyboard)
  d["plate"]   = "";   // nomor plat — diisi/diedit dari web
  d["idx_label"] = (selectedIndex>=0) ? cfg.indices[selectedIndex].label : String("?");
  d["th_hc"] = (selectedIndex>=0) ? cfg.indices[selectedIndex].th_hc : 0;
  d["th_co"] = (selectedIndex>=0) ? cfg.indices[selectedIndex].th_co : 0;
  d["avg_hc"]= resAvgHC;
  d["avg_co"]= resAvgCO;
  d["label"] = resultLabel;
  d["code"]  = resultCode;
  d["n"]     = sampledCount;
  // samples
  JsonArray hcArr = d["s_hc"].to<JsonArray>();
  JsonArray coArr = d["s_co"].to<JsonArray>();
  uint16_t lim = min(sampledCount, (uint16_t)MAX_SAMPLES);
  for (uint16_t i = 0; i < lim; i++) {
    hcArr.add(sampleHC[i]);
    coArr.add(sampleCO[i]);
  }

  size_t n = writeLogLine(d);
  if (n == 0) {
    Serial.println("[LOG] write fail -> remount SD & retry...");
    SD.end();
    delay(50);
    if (SD.begin(SD_CS, SPI, 4000000)) {
      n = writeLogLine(d);
    } else {
      Serial.println("[LOG] remount fail");
      sd_ok = false;   // tandai agar terlihat di /api/status & web
    }
  }
  lastLogSaved = (n > 0);
  size_t fsize = 0;
  File chk = SD.open(LOG_PATH, FILE_READ);
  if (chk) { fsize = chk.size(); chk.close(); }
  Serial.printf("[LOG] %s (%u bytes, file=%u bytes)\n",
                lastLogSaved ? "saved" : "SAVE FAIL", (unsigned)n, (unsigned)fsize);
}

// Edit metadata (vehicle & plate) 1 entry log berdasarkan id (= indeks baris).
// Rewrite streaming baris-per-baris ke file temp lalu rename — hemat heap walau
// log besar (tidak memuat seluruh file ke RAM, hanya 1 baris target yang di-parse).
bool editLogEntry(int wantId, const String& vehicle, const String& plate) {
  if (!sd_ok || !SD.exists(LOG_PATH)) return false;
  File in = SD.open(LOG_PATH, FILE_READ);
  if (!in) return false;
  if (SD.exists(LOG_TMP)) SD.remove(LOG_TMP);
  File out = SD.open(LOG_TMP, FILE_WRITE);
  if (!out) { in.close(); return false; }

  int id = 0;
  bool edited = false;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (id == wantId) {
      JsonDocument d;
      if (!deserializeJson(d, line)) {
        d["vehicle"] = vehicle;
        d["plate"]   = plate;
        serializeJson(d, out);
        edited = true;
      } else {
        out.print(line);   // baris korup: tulis apa adanya
      }
    } else {
      out.print(line);
    }
    out.print('\n');
    id++;
  }
  in.close();
  out.close();
  if (!edited) { SD.remove(LOG_TMP); return false; }
  SD.remove(LOG_PATH);
  return SD.rename(LOG_TMP, LOG_PATH);
}

// ---------------- State machine transitions ----------------
void enterState(SysState ns) {
  state = ns;
  phaseStart = millis();
  switch(ns){
    case ST_INHALE:
      phaseDuration = cfg.inhale_ms;
      setMotor(true);
      chartReset();   // chart reset di awal cycle
      setBuzzerPattern(2); // transition
      break;
    case ST_SELECT_IDX:
      setMotor(false);
      setBuzzerPattern(0);
      selectedIndex = -1;
      break;
    case ST_PREPROCESS:
      phaseDuration = cfg.preprocess_ms;
      setMotor(false);
      setBuzzerPattern(2);
      break;
    case ST_SAMPLING:
      phaseDuration = (uint32_t)cfg.sample_interval_ms * cfg.sample_count;
      sumHC = sumCO = 0; sampledCount = 0; lastSampleAt = 0;
      setMotor(false);
      setBuzzerPattern(2);
      break;
    case ST_RESULT:
      setMotor(false);
      classify();
      setBuzzerPattern(3); // long beep
      saveLogEntry();      // simpan ke SD dulu -> status ikut di frame result
      broadcastResult();
      break;
    case ST_IDLE:
      setMotor(false);
      flushManual = false;
      setBuzzerPattern(0);
      buzzerOff();
      selectedIndex = -1;
      break;
    case ST_WAIT_VALVE:
      setMotor(false);
      setBuzzerPattern(0);
      break;
  }
  broadcastState();
}

void tickStateMachine() {
  uint32_t now = millis();
  // Pasang running-pattern (2 bip/detik) HANYA jika tidak ada pattern lain berjalan.
  // Pattern 2 (transition) dan 3 (long beep) harus selesai dulu — biar tidak ketimpa.
  if (state==ST_INHALE || state==ST_PREPROCESS || state==ST_SAMPLING) {
    if (buzzerPattern == 0) setBuzzerPattern(1);
  }

  switch(state){
    case ST_INHALE:
      if (now - phaseStart >= phaseDuration) enterState(ST_PREPROCESS);
      break;
    case ST_PREPROCESS:
      if (now - phaseStart >= phaseDuration) enterState(ST_SAMPLING);
      break;
    case ST_SAMPLING:
      if (lastSampleAt == 0 || now - lastSampleAt >= cfg.sample_interval_ms) {
        lastSampleAt = now;
        sumHC += lastHC;
        sumCO += lastCO;
        if (sampledCount < MAX_SAMPLES) {
          sampleHC[sampledCount] = lastHC;
          sampleCO[sampledCount] = lastCO;
        }
        sampledCount++;
        if (sampledCount >= cfg.sample_count) enterState(ST_RESULT);
      }
      break;
    default: break;
  }
}

// ---------------- WS command handling ----------------
void handleCmd(JsonDocument& doc) {
  String cmd = doc["cmd"] | "";
  if (cmd == "request_start") {
    // Mulai flow: IDLE -> SELECT_IDX (lalu user pilih index via web/TFT)
    if (state == ST_IDLE) {
      if (cfg.indices.empty()) return;
      idxScrollOffset = 0;
      enterState(ST_SELECT_IDX);
    }
  } else if (cmd == "start") {
    // Legacy: terima index langsung
    if (state == ST_IDLE || state == ST_SELECT_IDX) {
      int idx = doc["index"] | -1;
      if (idx < 0 || idx >= (int)cfg.indices.size()) return;
      selectedIndex = idx;
      enterState(ST_WAIT_VALVE);
    }
  } else if (cmd == "select_idx") {
    if (state == ST_SELECT_IDX) {
      int idx = doc["index"] | -1;
      if (idx < 0 || idx >= (int)cfg.indices.size()) return;
      selectedIndex = idx;
      enterState(ST_WAIT_VALVE);
    }
  } else if (cmd == "confirm_valve") {
    if (state == ST_WAIT_VALVE) enterState(ST_INHALE);
  } else if (cmd == "stop") {
    enterState(ST_IDLE);
  } else if (cmd == "flush") {
    bool on = doc["on"] | false;
    if (state == ST_IDLE) {
      Serial.printf("[FLUSH] %s  heap=%u\n", on?"ON":"OFF", ESP.getFreeHeap());
      flushManual = on;
      setMotor(on);
      broadcastState();
      // repaint tombol flush ditangani tftUpdateIdleValues (partial, ringan)
    }
  } else if (cmd == "ping") {
    JsonDocument r; r["type"]="pong"; String s; serializeJson(r,s); wsSend(s);
  }
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len){
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index==0 && info->len==len && info->opcode==WS_TEXT){
      JsonDocument doc;
      if (!deserializeJson(doc, data, len)) handleCmd(doc);
    }
  }
}

// ---------------- HTTP handlers ----------------
void buildSettingsJson(String& out) {
  JsonDocument d;
  d["inhale_ms"] = cfg.inhale_ms;
  d["preprocess_ms"] = cfg.preprocess_ms;
  d["sample_interval_ms"] = cfg.sample_interval_ms;
  d["sample_count"] = cfg.sample_count;
  d["r0_mq2"] = cfg.r0_mq2;
  d["r0_mq7"] = cfg.r0_mq7;
  JsonArray arr = d["indices"].to<JsonArray>();
  for (auto& e : cfg.indices) {
    JsonObject o = arr.add<JsonObject>();
    o["label"]=e.label; o["th_hc"]=e.th_hc; o["th_co"]=e.th_co;
  }
  serializeJson(d, out);
}

void registerRoutes() {
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req){
    String s; buildSettingsJson(s);
    req->send(200, "application/json", s);
  });

  AsyncCallbackJsonWebHandler* h = new AsyncCallbackJsonWebHandler("/api/settings",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      JsonObject o = json.as<JsonObject>();
      if (!o["inhale_ms"].isNull())          cfg.inhale_ms          = max((uint32_t)100,  (uint32_t)o["inhale_ms"]);
      if (!o["preprocess_ms"].isNull())      cfg.preprocess_ms      = max((uint32_t)100,  (uint32_t)o["preprocess_ms"]);
      if (!o["sample_interval_ms"].isNull()) cfg.sample_interval_ms = max((uint32_t)100,  (uint32_t)o["sample_interval_ms"]);
      if (!o["sample_count"].isNull())       cfg.sample_count       = max((uint16_t)1,    (uint16_t)o["sample_count"]);
      if (!o["r0_mq2"].isNull())             cfg.r0_mq2             = max(1.0f,           (float)o["r0_mq2"]);
      if (!o["r0_mq7"].isNull())             cfg.r0_mq7             = max(1.0f,           (float)o["r0_mq7"]);
      if (!o["indices"].isNull()) {
        cfg.indices.clear();
        for (JsonObject ie : o["indices"].as<JsonArray>()) {
          IndexEntry e;
          e.label = ie["label"].as<String>();
          e.th_hc = ie["th_hc"] | 500.0f;
          e.th_co = ie["th_co"] | 2.0f;
          cfg.indices.push_back(e);
        }
      }
      saveSettings();
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(h);

  // POST /api/calibrate?hc=<ppm>&co=<persen> -> MULAI kalibrasi target idle dinamis.
  // Non-blocking: hanya memicu start (sampling dicicil di loop), respon instan.
  // Hasil dikirim via WebSocket type=calib_done; fallback: GET /api/calibrate.
  // Tanpa parameter: pakai default CALIB_TARGET_HC_PPM / CALIB_TARGET_CO_PPM.
  server.on("/api/calibrate", HTTP_POST, [](AsyncWebServerRequest* req){
    if (state != ST_IDLE) {
      req->send(409, "application/json", "{\"ok\":false,\"err\":\"hanya bisa di state IDLE\"}");
      return;
    }
    if (calibActive) {
      req->send(409, "application/json", "{\"ok\":false,\"err\":\"kalibrasi sedang berjalan\"}");
      return;
    }
    float targetHc = CALIB_TARGET_HC_PPM;            // ppm
    float targetCo = CALIB_TARGET_CO_PPM;            // ppm (dari % x 10000)
    if (req->hasParam("hc")) targetHc = req->getParam("hc")->value().toFloat();
    if (req->hasParam("co")) targetCo = req->getParam("co")->value().toFloat() * 10000.0f;
    if (!(targetHc >= 1.0f && targetHc <= 50000.0f) ||
        !(targetCo >= 100.0f && targetCo <= 100000.0f)) {
      req->send(400, "application/json",
        "{\"ok\":false,\"err\":\"target invalid (HC 1-50000 ppm, CO 0.01-10 %)\"}");
      return;
    }
    calibStart(targetHc, targetCo);
    char body[96];
    snprintf(body, sizeof(body),
      "{\"ok\":true,\"started\":true,\"duration_ms\":%u}",
      (unsigned)(CALIB_SAMPLES * 200u));
    req->send(200, "application/json", body);
  });

  // GET /api/calibrate -> status & hasil terakhir (fallback jika frame WS ter-drop)
  server.on("/api/calibrate", HTTP_GET, [](AsyncWebServerRequest* req){
    String s = "{\"active\":";
    s += calibActive ? "true" : "false";
    s += ",\"result\":";
    s += calibResultJson.length() ? calibResultJson : "null";
    s += "}";
    req->send(200, "application/json", s);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req){
    JsonDocument d;
    d["state"] = stateName(state);
    d["motor"] = motorOn;
    d["mq2_ok"] = mq2_ok;
    d["mq7_ok"] = mq7_ok;
    d["tft_ok"] = tft_ok;
    d["sd_ok"] = sd_ok;
    d["log_saved"] = lastLogSaved;   // status simpan log run terakhir
    d["rssi"] = WiFi.RSSI();
    d["ip"] = WiFi.localIP().toString();
    d["heap"] = ESP.getFreeHeap();
    d["uptime_ms"] = (uint32_t)millis();
    d["adc_mq2"] = lastAdcMQ2;
    d["adc_mq7"] = lastAdcMQ7;
    d["hc"] = lastHC;
    d["co"] = lastCO;
    String s; serializeJson(d, s);
    req->send(200, "application/json", s);
  });

  // === LOG endpoints (SD card) ===
  // GET /api/logs        -> daftar ringkasan (tanpa array sample) untuk tabel
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!sd_ok) { req->send(503, "application/json", "{\"err\":\"sd_not_ready\"}"); return; }
    if (!SD.exists(LOG_PATH)) { req->send(200, "application/json", "[]"); return; }
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) { req->send(500, "application/json", "{\"err\":\"open_fail\"}"); return; }
    JsonDocument outDoc;
    JsonArray arr = outDoc.to<JsonArray>();
    int id = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      JsonDocument d;
      if (deserializeJson(d, line)) { id++; continue; }
      JsonObject o = arr.add<JsonObject>();
      o["id"]      = id;
      o["ts"]      = d["ts"].as<String>();
      o["vehicle"] = d["vehicle"] | "";   // entry lama tanpa field -> string kosong
      o["plate"]   = d["plate"]   | "";
      o["idx"]    = d["idx_label"].as<String>();
      o["avg_hc"] = d["avg_hc"].as<float>();
      o["avg_co"] = d["avg_co"].as<float>();
      o["code"]   = d["code"].as<int>();
      o["label"]  = d["label"].as<String>();
      o["n"]      = d["n"].as<int>();
      id++;
    }
    f.close();
    String out;
    serializeJson(outDoc, out);
    req->send(200, "application/json", out);
  });

  // GET /api/log?id=N    -> detail lengkap satu log (termasuk samples)
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!sd_ok) { req->send(503, "application/json", "{\"err\":\"sd_not_ready\"}"); return; }
    if (!req->hasParam("id")) { req->send(400, "application/json", "{\"err\":\"need_id\"}"); return; }
    int wantId = req->getParam("id")->value().toInt();
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) { req->send(404, "application/json", "{\"err\":\"no_log\"}"); return; }
    int id = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      if (id == wantId) {
        f.close();
        req->send(200, "application/json", line);
        return;
      }
      id++;
    }
    f.close();
    req->send(404, "application/json", "{\"err\":\"id_not_found\"}");
  });

  // POST /api/log/edit?id=N  body {vehicle, plate} -> edit metadata 1 entry
  AsyncCallbackJsonWebHandler* eh = new AsyncCallbackJsonWebHandler("/api/log/edit",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      if (!sd_ok) { req->send(503, "application/json", "{\"ok\":false,\"err\":\"sd_not_ready\"}"); return; }
      if (!req->hasParam("id")) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"need_id\"}"); return; }
      int wantId = req->getParam("id")->value().toInt();
      JsonObject o = json.as<JsonObject>();
      String vehicle = o["vehicle"] | "";
      String plate   = o["plate"]   | "";
      vehicle.trim(); plate.trim();
      if (vehicle.length() > 40) vehicle = vehicle.substring(0, 40);
      if (plate.length()   > 20) plate   = plate.substring(0, 20);
      if (editLogEntry(wantId, vehicle, plate)) {
        req->send(200, "application/json", "{\"ok\":true}");
      } else {
        req->send(404, "application/json", "{\"ok\":false,\"err\":\"id_not_found\"}");
      }
    });
  server.addHandler(eh);

  // DELETE /api/logs     -> hapus semua log
  server.on("/api/logs", HTTP_DELETE, [](AsyncWebServerRequest* req){
    if (!sd_ok) { req->send(503, "application/json", "{\"err\":\"sd_not_ready\"}"); return; }
    if (SD.exists(LOG_PATH)) SD.remove(LOG_PATH);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/logs/download -> raw JSONL (untuk download/backup)
  server.on("/api/logs/download", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!sd_ok || !SD.exists(LOG_PATH)) { req->send(404, "text/plain", "no logs"); return; }
    req->send(SD, LOG_PATH, "application/x-ndjson", true);
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest* req){ req->send(404, "text/plain", "Not Found"); });
}

// ============================================================
//                  TFT  +  TOUCH  +  CHART
// ============================================================
uint16_t resultColor(uint8_t code){
  switch(code){
    case 0: return ILI9341_GREEN;
    case 1: return ILI9341_YELLOW;
    case 2: return ILI9341_ORANGE;
    case 3: return ILI9341_RED;
  }
  return ILI9341_WHITE;
}

// ---------- Touch button (rect region + label) ----------
void drawBtn(TBtn b, const char* label, uint16_t bg, uint16_t fg, uint16_t border, uint8_t size=2) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, border);
  tft.setTextColor(fg); tft.setTextSize(size);
  int tw = (int)strlen(label) * 6 * size;
  int th = 8 * size;
  tft.setCursor(b.x + (b.w - tw)/2, b.y + (b.h - th)/2);
  tft.print(label);
}
bool tapped(TBtn b, int tx, int ty) {
  return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

// ---------- Touch (edge-detected: 1 aksi per sekali tekan) ----------
// State machine internal:
//   - touchDown  : true jika jari sedang menyentuh (sudah ter-register)
//   - releasedAt : kapan terakhir terdeteksi LEPAS
// Sebuah tap baru hanya valid jika: (a) sebelumnya sudah lepas >= TOUCH_RELEASE_MS,
// dan (b) sudah lewat TOUCH_DEBOUNCE dari tap sebelumnya. Selama jari ditahan,
// getTouch() tidak akan trigger berulang.
bool getTouch(int* x, int* y) {
  static bool     touchDown = false;
  static uint32_t lastTapAt = 0;
  static uint32_t releasedAt = 0;

  uint32_t now = millis();
  bool pressed = ts.touched();

  if (!pressed) {
    // jari lepas
    if (touchDown) releasedAt = now;   // catat momen lepas (rising-to-low edge)
    touchDown = false;
    return false;
  }

  // pressed == true
  if (touchDown) return false;          // masih ditahan dari tap sebelumnya → abaikan

  // ini calon tap baru (rising edge low->high)
  if (now - releasedAt < TOUCH_RELEASE_MS) return false; // belum cukup lama lepas (bounce)
  if (now - lastTapAt  < TOUCH_DEBOUNCE)   return false; // terlalu cepat dari tap terakhir

  TS_Point p = ts.getPoint();
  if (p.z < TOUCH_PRESS_MIN) return false;   // tekanan terlalu lemah → bukan tap valid

  // tap valid
  touchDown = true;
  lastTapAt = now;
  *x = constrain(map(p.x, TOUCH_XMAX, TOUCH_XMIN, 0, 320), 0, 319);
  *y = constrain(map(p.y, TOUCH_YMAX, TOUCH_YMIN, 0, 240), 0, 239);
  return true;
}

// ---------- Chart ----------
#define CHART_X  6
#define CHART_Y  140
#define CHART_W  308
#define CHART_H  70

void chartReset() { chartHead = 0; chartCount = 0; }
void chartPush(float hc, float co) {
  chartHC[chartHead] = hc;
  chartCO[chartHead] = co;
  chartHead = (chartHead + 1) % CHART_N;
  if (chartCount < CHART_N) chartCount++;
}
void chartDrawFrame() {
  tft.drawRoundRect(CHART_X, CHART_Y, CHART_W, CHART_H, 4, ILI9341_DARKGREY);
}
void chartDraw() {
  // bersihkan area dalam
  tft.fillRect(CHART_X+1, CHART_Y+1, CHART_W-2, CHART_H-2, ILI9341_BLACK);
  // baseline grid lines (3 horizontal)
  for (int i = 1; i < 3; i++) {
    int yg = CHART_Y + (CHART_H * i)/3;
    for (int x = CHART_X+4; x < CHART_X+CHART_W-4; x += 4)
      tft.drawPixel(x, yg, 0x2104);
  }
  // legenda kecil di pojok kiri-atas dalam chart
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_BLUE);  tft.setCursor(CHART_X + 4, CHART_Y + 4); tft.print("HC");
  tft.setTextColor(ILI9341_RED);   tft.setCursor(CHART_X + 28, CHART_Y + 4); tft.print("CO");

  if (chartCount < 2) return;

  // hitung min/max
  float hcMin = 1e9, hcMax = -1e9, coMin = 1e9, coMax = -1e9;
  int start = (chartHead - chartCount + CHART_N) % CHART_N;
  for (int i = 0; i < chartCount; i++) {
    int k = (start + i) % CHART_N;
    if (chartHC[k] < hcMin) hcMin = chartHC[k];
    if (chartHC[k] > hcMax) hcMax = chartHC[k];
    if (chartCO[k] < coMin) coMin = chartCO[k];
    if (chartCO[k] > coMax) coMax = chartCO[k];
  }
  if (hcMax - hcMin < 1.0f)  hcMax = hcMin + 1.0f;
  if (coMax - coMin < 0.05f) coMax = coMin + 0.05f;

  int innerH = CHART_H - 8;
  int innerY = CHART_Y + 6;
  int innerX = CHART_X + 2;
  int innerW = CHART_W - 4;

  // gambar 2 garis polyline
  int prevX = 0, prevYH = 0, prevYC = 0;
  for (int i = 0; i < chartCount; i++) {
    int k = (start + i) % CHART_N;
    int x  = innerX + (int)((float)i * (innerW - 1) / (float)(CHART_N - 1));
    int yh = innerY + innerH - (int)((chartHC[k] - hcMin) / (hcMax - hcMin) * innerH);
    int yc = innerY + innerH - (int)((chartCO[k] - coMin) / (coMax - coMin) * innerH);
    if (i > 0) {
      tft.drawLine(prevX, prevYH, x, yh, ILI9341_BLUE);
      tft.drawLine(prevX, prevYC, x, yc, ILI9341_RED);
    }
    prevX = x; prevYH = yh; prevYC = yc;
  }
}

// ---------- Common header ----------
void tftHeader() {
  tft.fillRect(0, 0, 320, 28, ILI9341_BLUE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.print("KNALPOT ANALYZER");
}

// ============================================================
// SCREEN: IDLE  (Start, Flush buttons + live sensors)
// ============================================================
// Tombol IDLE
TBtn btnIdleStart = {10, 188, 150, 44};
TBtn btnIdleFlush = {170,188, 140, 44};

void tftDrawIdle() {
  tft.fillScreen(ILI9341_BLACK);
  tftHeader();
  // STATE strip
  tft.fillRect(0, 28, 320, 22, 0x18C3);
  tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
  tft.setCursor(10, 33); tft.print("STATE:");
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(100, 33); tft.print("IDLE - SIAP");

  // HC / CO labels
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(2);
  tft.setCursor(10, 60);  tft.print("HC");
  tft.setCursor(170, 60); tft.print("CO");
  tft.setTextSize(1);
  tft.setCursor(10, 80);  tft.print("Hidrokarbon (ppm)");
  tft.setCursor(170, 80); tft.print("Karbon Monoksida (%)");
  tft.drawLine(160, 56, 160, 130, ILI9341_DARKGREY);
  tft.drawLine(0, 134, 320, 134, ILI9341_DARKGREY);

  // status row (sensor + WiFi)
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(10,  142); tft.print("MQ2:");
  tft.setCursor(80,  142); tft.print("MQ7:");
  tft.setCursor(150, 142); tft.print("WiFi:");
  tft.setCursor(230, 142); tft.print("IP:");

  // Tombols
  drawBtn(btnIdleStart, "START", ILI9341_GREEN,  ILI9341_BLACK, ILI9341_WHITE);
  drawBtn(btnIdleFlush, flushManual?"FLUSH:ON":"FLUSH:OFF",
          flushManual?ILI9341_ORANGE:ILI9341_DARKGREY,
          ILI9341_WHITE, ILI9341_WHITE);

  tftUpdateIdleValues();
}

void tftUpdateIdleValues() {
  // Re-paint flush button jika state berubah (sync dari web)
  static int8_t lastFlushDrawn = -1;
  if ((int8_t)flushManual != lastFlushDrawn) {
    drawBtn(btnIdleFlush, flushManual?"FLUSH:ON":"FLUSH:OFF",
            flushManual?ILI9341_ORANGE:ILI9341_DARKGREY,
            ILI9341_WHITE, ILI9341_WHITE);
    lastFlushDrawn = flushManual;
  }
  // HC value
  tft.fillRect(10, 95, 145, 32, ILI9341_BLACK);
  tft.setTextColor(mq2_ok?ILI9341_GREEN:ILI9341_RED); tft.setTextSize(4);
  tft.setCursor(10, 100); tft.printf("%.0f", lastHC);
  // CO value
  tft.fillRect(170, 95, 145, 32, ILI9341_BLACK);
  tft.setTextColor(mq7_ok?ILI9341_ORANGE:ILI9341_RED); tft.setTextSize(4);
  tft.setCursor(170, 100); tft.printf("%.2f", lastCO);

  // status row dynamic
  tft.fillRect(40, 142, 35, 10, ILI9341_BLACK);
  tft.fillRect(110,142, 35, 10, ILI9341_BLACK);
  tft.fillRect(186,142, 40, 10, ILI9341_BLACK);
  tft.fillRect(250,142, 70, 10, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(mq2_ok?ILI9341_GREEN:ILI9341_RED);
  tft.setCursor(40, 142); tft.print(mq2_ok?"OK":"DSC");
  tft.setTextColor(mq7_ok?ILI9341_GREEN:ILI9341_RED);
  tft.setCursor(110,142); tft.print(mq7_ok?"OK":"DSC");
  int rssi = WiFi.RSSI();
  uint16_t wcol = (rssi > -60)?ILI9341_GREEN:(rssi > -75)?ILI9341_YELLOW:ILI9341_RED;
  tft.setCursor(186, 142);
  if (WiFi.status()==WL_CONNECTED) { tft.setTextColor(wcol); tft.printf("%d", rssi); }
  else { tft.setTextColor(ILI9341_RED); tft.print("OFF"); }
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(250, 142);
  tft.print(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("-"));

  // hint line above buttons
  tft.fillRect(10, 165, 300, 14, ILI9341_BLACK);
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(10, 168); tft.print("Tekan START di TFT / web (bayu.local)");
}

// ============================================================
// SCREEN: SELECT INDEX (scrollable list)
// ============================================================
#define IDX_ITEM_H   42
#define IDX_VISIBLE  3
#define IDX_LIST_X   10
#define IDX_LIST_Y   60
#define IDX_LIST_W   250
TBtn btnIdxUp   = {270, 60,  40, 60};
TBtn btnIdxDown = {270, 132, 40, 60};
TBtn btnIdxBack = {10, 198, 100, 36};

void tftDrawSelectIdx() {
  tft.fillScreen(ILI9341_BLACK);
  tftHeader();
  tft.fillRect(0, 28, 320, 22, 0x18C3);
  tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
  tft.setCursor(10, 33); tft.print("PILIH INDEX TAHUN");

  // items
  int n = (int)cfg.indices.size();
  if (idxScrollOffset > max(0, n - IDX_VISIBLE)) idxScrollOffset = max(0, n - IDX_VISIBLE);
  if (idxScrollOffset < 0) idxScrollOffset = 0;
  for (int v = 0; v < IDX_VISIBLE; v++) {
    int i = idxScrollOffset + v;
    int y = IDX_LIST_Y + v * IDX_ITEM_H;
    if (i >= n) {
      tft.fillRoundRect(IDX_LIST_X, y, IDX_LIST_W, IDX_ITEM_H - 4, 4, 0x10A2);
      continue;
    }
    tft.fillRoundRect(IDX_LIST_X, y, IDX_LIST_W, IDX_ITEM_H - 4, 4, 0x2104);
    tft.drawRoundRect(IDX_LIST_X, y, IDX_LIST_W, IDX_ITEM_H - 4, 4, ILI9341_DARKGREY);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(2);
    tft.setCursor(IDX_LIST_X + 8, y + 5); tft.print(cfg.indices[i].label);
    tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
    tft.setCursor(IDX_LIST_X + 8, y + 24);
    tft.printf("HC>=%g ppm  CO>=%g %%", cfg.indices[i].th_hc, cfg.indices[i].th_co);
  }

  // up/down
  drawBtn(btnIdxUp,   "^", ILI9341_DARKGREY, ILI9341_WHITE, ILI9341_WHITE, 3);
  drawBtn(btnIdxDown, "v", ILI9341_DARKGREY, ILI9341_WHITE, ILI9341_WHITE, 3);

  // back
  drawBtn(btnIdxBack, "BACK", ILI9341_RED, ILI9341_WHITE, ILI9341_WHITE);

  // page indicator (di antara list dan tombol)
  tft.fillRect(120, 186, 180, 10, ILI9341_BLACK);
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(120, 186);
  int start1 = idxScrollOffset + 1;
  int end1   = min(idxScrollOffset + IDX_VISIBLE, n);
  if (n == 0) tft.print("Tidak ada index (atur di Settings)");
  else        tft.printf("Menampilkan %d-%d dari %d", start1, end1, n);
}

// ============================================================
// SCREEN: WAIT VALVE
// ============================================================
TBtn btnValveOK     = {170,180, 140, 50};
TBtn btnValveCancel = {10, 180, 140, 50};

void tftValveNotice() {
  tft.fillScreen(ILI9341_BLACK);
  tftHeader();
  // warning bar
  tft.fillRect(0, 28, 320, 24, ILI9341_ORANGE);
  tft.setTextColor(ILI9341_BLACK); tft.setTextSize(2);
  tft.setCursor(10, 33); tft.print("KONFIRMASI KATUP");

  tft.setTextColor(ILI9341_YELLOW); tft.setTextSize(3);
  tft.setCursor(10, 65);  tft.print("PASTIKAN");
  tft.setCursor(10, 100); tft.print("KATUP SELANG");
  tft.setCursor(10, 135); tft.print("TERTUTUP");
  if (selectedIndex >= 0) {
    tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
    tft.setCursor(10, 168); tft.print("Index: ");
    tft.print(cfg.indices[selectedIndex].label);
  }
  drawBtn(btnValveCancel, "BATAL",      ILI9341_DARKGREY, ILI9341_WHITE, ILI9341_WHITE);
  drawBtn(btnValveOK,     "KONFIRMASI", ILI9341_GREEN,    ILI9341_BLACK, ILI9341_WHITE);
}

// ============================================================
// SCREEN: RUN (INHALE / PREPROCESS / SAMPLING) - with chart
// ============================================================
TBtn btnRunStop = {220, 215, 90, 22};

void tftDrawRun() {
  tft.fillScreen(ILI9341_BLACK);
  tftHeader();
  // state bar
  tft.fillRect(0, 28, 320, 22, 0x18C3);
  tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
  tft.setCursor(10, 33); tft.print("STATE:");
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(100, 33); tft.print(stateName(state));

  // labels HC/CO
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(10, 58);  tft.print("HC (ppm)");
  tft.setCursor(170, 58); tft.print("CO (%)");
  tft.drawLine(160, 56, 160, 130, ILI9341_DARKGREY);

  // progress bar frame
  tft.drawRoundRect(10, 215, 200, 22, 4, ILI9341_WHITE);
  // Stop button
  drawBtn(btnRunStop, "STOP", ILI9341_RED, ILI9341_WHITE, ILI9341_WHITE, 1);

  // chart frame
  chartDrawFrame();
}

void tftUpdateValues() {
  // angka HC/CO besar
  tft.fillRect(10, 70, 145, 60, ILI9341_BLACK);
  tft.fillRect(170,70, 145, 60, ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN); tft.setTextSize(4);
  tft.setCursor(10, 78); tft.printf("%.0f", lastHC);
  tft.setTextColor(ILI9341_ORANGE); tft.setTextSize(4);
  tft.setCursor(170, 78); tft.printf("%.2f", lastCO);

  // chart redraw
  chartDraw();

  // progress fill
  tft.fillRect(11, 216, 198, 20, ILI9341_BLACK);
  if (phaseDuration > 0) {
    uint32_t el = millis() - phaseStart;
    if (el > phaseDuration) el = phaseDuration;
    int w = (int)((el * 198UL) / phaseDuration);
    tft.fillRect(11, 216, w, 20, ILI9341_CYAN);
  }
  // counter overlay
  tft.setTextSize(1); tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(15, 222);
  if (state==ST_SAMPLING) tft.printf("Sample: %u / %u", sampledCount, cfg.sample_count);
  else if (phaseDuration>0) tft.printf("%lu / %lu ms", millis()-phaseStart, phaseDuration);
}

// ============================================================
// SCREEN: RESULT
// ============================================================
TBtn btnResBack   = {10, 200, 145, 36};
TBtn btnResRetest = {165,200, 145, 36};

void tftDrawResult() {
  uint16_t col = resultColor(resultCode);
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, 320, 38, col);
  tft.setTextColor(ILI9341_BLACK); tft.setTextSize(2);
  tft.setCursor(10, 11); tft.print("HASIL RULE BASED AI");

  // result label (wrap)
  tft.setTextColor(col); tft.setTextSize(2);
  String s = resultLabel;
  int y = 48;
  while (s.length()) {
    int n = min((int)s.length(), 24);
    String line = s.substring(0, n);
    tft.setCursor(10, y); tft.print(line);
    s = s.substring(line.length());
    y += 22;
  }
  // AVG box
  tft.drawRoundRect(10, 110, 145, 80, 5, ILI9341_BLUE);
  tft.setTextColor(ILI9341_BLUE); tft.setTextSize(1);
  tft.setCursor(18, 116); tft.print("AVG HC");
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3);
  tft.setCursor(18, 130); tft.printf("%.1f", resAvgHC);
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(18, 158); tft.print("ppm");
  if (selectedIndex>=0){
    tft.setCursor(18, 175); tft.printf("th: %.0f", cfg.indices[selectedIndex].th_hc);
  }

  tft.drawRoundRect(165, 110, 145, 80, 5, ILI9341_RED);
  tft.setTextColor(ILI9341_RED); tft.setTextSize(1);
  tft.setCursor(173, 116); tft.print("AVG CO");
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3);
  tft.setCursor(173, 130); tft.printf("%.2f", resAvgCO);
  tft.setTextColor(ILI9341_DARKGREY); tft.setTextSize(1);
  tft.setCursor(173, 158); tft.print("%");
  if (selectedIndex>=0){
    tft.setCursor(173, 175); tft.printf("th: %.2f", cfg.indices[selectedIndex].th_co);
  }

  drawBtn(btnResBack,   "BACK IDLE", ILI9341_BLUE,  ILI9341_WHITE, ILI9341_WHITE, 1);
  drawBtn(btnResRetest, "TES ULANG", ILI9341_GREEN, ILI9341_BLACK, ILI9341_WHITE, 1);
}

// ============================================================
// REFRESH dispatcher
// ============================================================
void tftRefresh() {
  if (state != lastTftState || tftDirty) {
    switch(state) {
      case ST_IDLE:       tftDrawIdle();      break;
      case ST_SELECT_IDX: tftDrawSelectIdx(); break;
      case ST_WAIT_VALVE: tftValveNotice();   break;
      case ST_RESULT:     tftDrawResult();    break;
      default:            tftDrawRun();       break;
    }
    lastTftState = state;
    tftDirty = false;
  }
  if (state==ST_INHALE || state==ST_PREPROCESS || state==ST_SAMPLING) {
    if (millis() - lastTftRefresh > 300) {
      tftUpdateValues();
      lastTftRefresh = millis();
    }
  } else if (state == ST_IDLE) {
    if (millis() - lastTftRefresh > 200) {
      tftUpdateIdleValues();
      lastTftRefresh = millis();
    }
  }
}

// ============================================================
// TOUCH dispatcher (panggil dari loop)
// ============================================================
void handleIdleTouch(int x, int y) {
  if (tapped(btnIdleStart, x, y)) {
    if (cfg.indices.empty()) return;
    idxScrollOffset = 0;
    enterState(ST_SELECT_IDX);
  } else if (tapped(btnIdleFlush, x, y)) {
    flushManual = !flushManual;
    Serial.printf("[FLUSH-TFT] %s  heap=%u\n", flushManual?"ON":"OFF", ESP.getFreeHeap());
    setMotor(flushManual);
    broadcastState();
    // Tidak full-redraw (berat). tftUpdateIdleValues() akan repaint tombol flush
    // saja (ringan) saat mendeteksi perubahan flushManual.
  }
}
void handleSelectIdxTouch(int x, int y) {
  int n = (int)cfg.indices.size();
  if (tapped(btnIdxBack, x, y)) {
    enterState(ST_IDLE);
    return;
  }
  if (tapped(btnIdxUp, x, y)) {
    if (idxScrollOffset > 0) { idxScrollOffset--; tftDirty = true; }
    return;
  }
  if (tapped(btnIdxDown, x, y)) {
    if (idxScrollOffset < n - IDX_VISIBLE) { idxScrollOffset++; tftDirty = true; }
    return;
  }
  // tap pada item list
  for (int v = 0; v < IDX_VISIBLE; v++) {
    int i = idxScrollOffset + v;
    if (i >= n) break;
    int iy = IDX_LIST_Y + v * IDX_ITEM_H;
    TBtn area = { IDX_LIST_X, iy, IDX_LIST_W, IDX_ITEM_H - 4 };
    if (tapped(area, x, y)) {
      selectedIndex = i;
      enterState(ST_WAIT_VALVE);
      return;
    }
  }
}
void handleValveTouch(int x, int y) {
  if (tapped(btnValveOK,     x, y)) enterState(ST_INHALE);
  else if (tapped(btnValveCancel, x, y)) enterState(ST_IDLE);
}
void handleRunTouch(int x, int y) {
  if (tapped(btnRunStop, x, y)) enterState(ST_IDLE);
}
void handleResultTouch(int x, int y) {
  if (tapped(btnResBack, x, y)) enterState(ST_IDLE);
  else if (tapped(btnResRetest, x, y)) {
    idxScrollOffset = 0;
    enterState(ST_SELECT_IDX);
  }
}
void dispatchTouch(int x, int y) {
  switch(state){
    case ST_IDLE:       handleIdleTouch(x, y); break;
    case ST_SELECT_IDX: handleSelectIdxTouch(x, y); break;
    case ST_WAIT_VALVE: handleValveTouch(x, y); break;
    case ST_INHALE:
    case ST_PREPROCESS:
    case ST_SAMPLING:   handleRunTouch(x, y); break;
    case ST_RESULT:     handleResultTouch(x, y); break;
  }
}

// ---------------- Setup / Loop ----------------
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) { delay(200); }
}

// ---- Boot screen helpers ----
// 9 slot baris, masing-masing 18px, mulai y=52  (52 + 9*18 = 214, footer di 222)
#define BOOT_Y0       52
#define BOOT_ROW_H    18
#define BOOT_X_LABEL  60
#define BOOT_X_STATUS 10
#define BOOT_X_DETAIL 60   // detail line for some slots

void bootDrawFrame() {
  tft.fillScreen(ILI9341_BLACK);
  tftHeader();
  // sub-title
  tft.fillRect(0, 28, 320, 22, 0x18C3);
  tft.setTextColor(ILI9341_CYAN); tft.setTextSize(2);
  tft.setCursor(10, 33); tft.print("SYSTEM BOOTING...");
}

// status: 0 = wait ".."  1 = OK   2 = FAIL
void bootStep(int row, const char* label, uint8_t status, const char* detail = nullptr) {
  int y = BOOT_Y0 + row * BOOT_ROW_H;
  tft.fillRect(0, y, 320, BOOT_ROW_H, ILI9341_BLACK);
  // status badge
  tft.setTextSize(1);
  tft.setCursor(BOOT_X_STATUS, y + 4);
  if (status == 1)      { tft.setTextColor(ILI9341_GREEN);  tft.print("[ OK ]"); }
  else if (status == 2) { tft.setTextColor(ILI9341_RED);    tft.print("[FAIL]"); }
  else                  { tft.setTextColor(ILI9341_YELLOW); tft.print("[ .. ]"); }
  // label
  tft.setTextColor(ILI9341_WHITE); tft.setTextSize(1);
  tft.setCursor(BOOT_X_LABEL, y + 4); tft.print(label);
  if (detail) {
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(BOOT_X_LABEL + 100, y + 4); tft.print(detail);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== BOOT ===");
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  // Diagnosa: kenapa ESP32 baru saja restart?
  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrStr;
  switch (rr) {
    case ESP_RST_POWERON:  rrStr = "POWERON (normal)"; break;
    case ESP_RST_SW:       rrStr = "SW (ESP.restart / reflash)"; break;
    case ESP_RST_PANIC:    rrStr = "PANIC (crash/exception - cek backtrace!)"; break;
    case ESP_RST_INT_WDT:  rrStr = "INT_WDT (interrupt watchdog)"; break;
    case ESP_RST_TASK_WDT: rrStr = "TASK_WDT (loop block > watchdog)"; break;
    case ESP_RST_WDT:      rrStr = "WDT (other watchdog)"; break;
    case ESP_RST_BROWNOUT: rrStr = "BROWNOUT (tegangan drop - kemungkinan relay/motor!)"; break;
    case ESP_RST_DEEPSLEEP:rrStr = "DEEPSLEEP"; break;
    case ESP_RST_EXT:      rrStr = "EXT (reset pin)"; break;
    default:               rrStr = "UNKNOWN"; break;
  }
  Serial.printf(">>> RESET REASON: %s\n", rrStr);

  // --- GPIO (sebelum TFT) ---
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_MOTOR,  OUTPUT); digitalWrite(PIN_MOTOR,  LOW);
  Serial.println("[1] GPIO ok");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);
  analogSetPinAttenuation(PIN_MQ7, ADC_11db);
  Serial.println("[2] ADC ok");

  // --- TFT + Touch init ---
  Serial.println("[3] SPI/TFT...");
  // Deselect ALL SPI slaves sebelum SPI.begin agar tidak ada bus contention saat init SD nanti
  pinMode(TFT_CS,   OUTPUT); digitalWrite(TFT_CS,   HIGH);
  pinMode(TOUCH_CS, OUTPUT); digitalWrite(TOUCH_CS, HIGH);
  pinMode(SD_CS,    OUTPUT); digitalWrite(SD_CS,    HIGH);
  SPI.begin(18, 19, 23, TFT_CS);
  tft.begin(TFT_SPI_HZ);   // clock TFT diturunkan agar tahan saat berbagi bus dgn touch
  tft.setRotation(1);
  tft_ok = true;
  ts.begin();
  ts.setRotation(1);
  bootDrawFrame();
  Serial.println("    TFT + Touch ok");

  // Pre-draw all boot rows in waiting state
  bootStep(0, "GPIO Init",    1);
  bootStep(1, "ADC Init",     1);
  bootStep(2, "TFT + Touch",  1);
  bootStep(3, "LittleFS",     0);
  bootStep(4, "Settings",     0);
  bootStep(5, "SD Card",      0);
  bootStep(6, "WiFi",         0);
  bootStep(7, "NTP+mDNS",     0);
  bootStep(8, "Web Server",   0);

  // --- LittleFS ---
  if (!LittleFS.begin(true)) {
    Serial.println("    LittleFS FAIL");
    bootStep(3, "LittleFS", 2);
  } else {
    Serial.println("    LittleFS OK");
    bootStep(3, "LittleFS", 1);
  }

  // --- Settings ---
  loadSettings();
  Serial.printf("    inhale=%u prep=%u sint=%u scnt=%u indices=%u\n",
    cfg.inhale_ms, cfg.preprocess_ms, cfg.sample_interval_ms, cfg.sample_count, cfg.indices.size());
  char detail[24]; snprintf(detail, sizeof(detail), "%u idx", (unsigned)cfg.indices.size());
  bootStep(4, "Settings", 1, detail);

  // --- SD Card ---
  Serial.println("    SD.begin...");
  // SD share SPI dengan TFT/touch. Pastikan slave lain deselect, lalu coba beberapa frekuensi.
  digitalWrite(TFT_CS,   HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(SD_CS,    HIGH);
  delay(10);
  sd_ok = false;
  const uint32_t sdFreqs[] = { 1000000, 4000000, 400000 };
  for (uint8_t i = 0; i < 3 && !sd_ok; i++) {
    SD.end();
    delay(20);
    if (SD.begin(SD_CS, SPI, sdFreqs[i])) { sd_ok = true; break; }
    Serial.printf("    SD retry (freq=%u) failed\n", (unsigned)sdFreqs[i]);
  }
  if (sd_ok) {
    uint64_t mb = SD.cardSize() / (1024*1024);
    char sdDetail[24]; snprintf(sdDetail, sizeof(sdDetail), "%llu MB", mb);
    Serial.printf("    SD OK (%llu MB)\n", mb);
    bootStep(5, "SD Card", 1, sdDetail);
  } else {
    Serial.println("    SD FAIL");
    bootStep(5, "SD Card", 2, "no card");
  }

  // --- WiFi ---
  Serial.println("    Connecting WiFi...");
  setupWiFi();
  bool wifiOk = (WiFi.status()==WL_CONNECTED);
  if (wifiOk) {
    Serial.print("    WiFi OK, IP: "); Serial.println(WiFi.localIP());
    bootStep(6, "WiFi", 1, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("    WiFi FAIL");
    bootStep(6, "WiFi", 2, WIFI_SSID);
  }

  // --- NTP + mDNS ---
  if (wifiOk) {
    // WIB = UTC+7, no DST
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
    struct tm ti;
    bool ntpOk = getLocalTime(&ti, 3000);
    bool mdnsOk = MDNS.begin("bayu");
    if (mdnsOk) MDNS.addService("http", "tcp", 80);
    char ntpDetail[24];
    if (ntpOk) {
      strftime(ntpDetail, sizeof(ntpDetail), "%H:%M:%S", &ti);
      Serial.printf("    NTP OK %s | mDNS %s\n", ntpDetail, mdnsOk?"ok":"fail");
    } else {
      snprintf(ntpDetail, sizeof(ntpDetail), "ntp fail");
      Serial.println("    NTP FAIL");
    }
    bootStep(7, "NTP+mDNS", (ntpOk && mdnsOk) ? 1 : 2, ntpDetail);
  } else {
    bootStep(7, "NTP+mDNS", 2, "skip");
  }

  // --- Web Server ---
  Serial.println("    Web server start...");
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  registerRoutes();
  server.begin();
  Serial.println("    Web OK");
  bootStep(8, "Web Server", 1, "port 80");

  // Footer "ready"
  tft.fillRect(0, 220, 320, 20, ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN); tft.setTextSize(2);
  tft.setCursor(80, 222); tft.print("SYSTEM READY");

  delay(1200);
  enterState(ST_IDLE);
  tftDrawIdle();
  lastTftState = ST_IDLE;
  Serial.println("=== READY ===");
}

uint32_t lastSensorRead = 0;
uint32_t lastBroadcast = 0;

void loop() {
  uint32_t now = millis();
  if (now - lastSensorRead > 200) {
    lastSensorRead = now;
    readSensors();
    // push ke chart hanya saat siklus berjalan
    if (state==ST_INHALE || state==ST_PREPROCESS || state==ST_SAMPLING) {
      chartPush(lastHC, lastCO);
    }
  }
  calibTick();   // kalibrasi non-blocking (no-op jika tidak aktif)
  // dispatch touch (di-throttle: kurangi kerapatan transaksi SPI touch vs TFT)
  static uint32_t lastTouchPoll = 0;
  if (now - lastTouchPoll >= TOUCH_POLL_MS) {
    lastTouchPoll = now;
    int tx, ty;
    if (getTouch(&tx, &ty)) {
      delayMicroseconds(50);   // beri jeda agar bus SPI touch settle sebelum TFT
      dispatchTouch(tx, ty);
    }
  }

  tickStateMachine();
  handleBuzzer();
  tftRefresh();
  if (now - lastBroadcast > 200) {
    lastBroadcast = now;
    broadcastData();
  }
  ws.cleanupClients();
}
