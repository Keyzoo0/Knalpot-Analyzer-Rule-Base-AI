# Sistem Monitoring Emisi Gas Buang Kendaraan Bermotor via Wi-Fi

ESP32-based exhaust gas analyzer untuk menilai kualitas pembakaran motor via **Rule Based AI** dari sensor MQ-2 (HC) dan MQ-7 (CO), dengan antarmuka **TFT touchscreen** + **dashboard web responsive** yang tersinkron real-time.

> Proyek Akhir — Anoki Bayu Defender (NIM 2241170160)
> Program Studi Sarjana Terapan Teknik Elektronika, Jurusan Teknik Elektro, Politeknik Negeri Malang — 2026

---

## 1. Fitur Utama

| Fitur | Keterangan |
|---|---|
| Sensor MQ-2 / MQ-7 | Pembacaan HC (ppm) dan CO (%) dengan voltage divider 4.7k/10k |
| State machine 7 state | IDLE → SELECT_IDX → WAIT_VALVE → INHALE → PREPROCESS → SAMPLING → RESULT |
| Rule Based AI | 4 kategori: Normal / Gejala Awal / Misfire / Tidak Normal |
| Threshold dinamis | Per "index tahun motor" — bisa ditambah, dihapus, edit via web |
| Web dashboard | 3 tab (Dashboard / Settings / Info) dengan Tailwind + Chart.js via CDN |
| TFT touchscreen | 5 layar (Idle / Select / Valve / Run / Result) dengan tombol on-screen |
| Chart real-time | Di web (Chart.js dual-axis) dan TFT (custom ring buffer 80 titik) |
| Sinkronisasi dua arah | Aksi dari TFT auto-update web, dan sebaliknya, via WebSocket |
| Buzzer non-blocking | 3 pola (running, transition, finish) berbasis `millis()` |
| Settings persistent | Tersimpan di NVS (`Preferences`), bertahan setelah reboot |
| mDNS | Akses lewat `http://bayu.local` selain via IP |

---

## 2. Hardware

### 2.1 Pin Map

```
ESP32 DevKit V1
┌─────────────────────────────────────────┐
│ GPIO 13 ─── Buzzer (active-HIGH)        │
│ GPIO 14 ─── Relay Motor (active-HIGH)   │
│ GPIO 18 ─── TFT SCK + Touch T_CLK       │
│ GPIO 19 ─── TFT MISO + Touch T_DO       │
│ GPIO 23 ─── TFT MOSI + Touch T_DIN      │
│ GPIO  5 ─── TFT CS                      │
│ GPIO  2 ─── TFT DC                      │
│ GPIO  4 ─── TFT RST                     │
│ GPIO 15 ─── Touch T_CS                  │
│ GPIO 26 ─── SD Card CS (share SPI bus)  │
│ GPIO 34 ─── MQ-7 (CO)  ── via R1/R2 ──┐ │
│ GPIO 35 ─── MQ-2 (HC)  ── via R1/R2 ──┤ │
└────────────────────────────────────────┘
                                          │
   Pembagi tegangan tiap MQ:              │
        Vout(5V) ──[R1=4.7k]──┬── GPIO34/35
                              │
                          [R2=10k]
                              │
                             GND
   Vadc = Vmodule × 10/(10+4.7) ≈ Vmodule × 0.68
   → 5V → ±3.4V (aman dalam range ADC 11dB ESP32)
```

### 2.2 Daftar Komponen

| Komponen | Kuantitas | Catatan |
|---|---|---|
| ESP32 DevKit V1 (30-pin) | 1 | |
| TFT ILI9341 2.4"/2.8" SPI + XPT2046 touch | 1 | Resolusi 320×240 |
| Sensor MQ-2 (modul) | 1 | Pengukuran HC/LPG |
| Sensor MQ-7 (modul) | 1 | Pengukuran CO (dengan heater siklus, lihat datasheet) |
| Relay 5V (active HIGH) atau MOSFET driver | 1 | Untuk motor diafragma 5–12V |
| Motor diafragma penghisap udara | 1 | Untuk hirup udara knalpot |
| Buzzer aktif 5V | 1 | |
| Resistor 4.7kΩ | 2 | R1 voltage divider |
| Resistor 10kΩ | 2 | R2 voltage divider |
| Power supply 5V/2A | 1 | Untuk motor + sensor |
| Kabel jumper, breadboard / PCB | — | |

---

## 3. Software Stack

### 3.1 Persyaratan

- **Arduino IDE 2.x** atau **arduino-cli**
- **ESP32 board package** (Espressif) — versi 3.x ke atas
- Plugin **Arduino LittleFS Upload** untuk upload folder `data/` ([repo](https://github.com/earlephilhower/arduino-littlefs-upload))

### 3.2 Library (install via Library Manager)

| Library | Sumber |
|---|---|
| **Adafruit GFX Library** | Adafruit |
| **Adafruit ILI9341** | Adafruit |
| **XPT2046_Touchscreen** | Paul Stoffregen |
| **ArduinoJson** v7.x | Benoît Blanchon |
| **ESPAsyncWebServer** | me-no-dev / ESP32Async |
| **AsyncTCP** | me-no-dev / ESP32Async |

Library lain (`WiFi`, `Preferences`, `LittleFS`, `SPI`, `ESPmDNS`) sudah include di ESP32 core.

### 3.3 Struktur File

```
bayu_knalpotSystem/
├── README.md                          ← dokumen ini
└── esp32Firmware/
    ├── esp32Firmware.ino              ← seluruh firmware (single file)
    └── data/                          ← di-upload ke LittleFS
        ├── index.html
        └── app.js
```

---

## 4. Build & Flash

### 4.1 Build firmware

1. Buka `esp32Firmware/esp32Firmware.ino` di Arduino IDE.
2. Pilih board: **Tools → Board → ESP32 Dev Module**.
3. Partition Scheme: **Default 4MB with spiffs** (atau apa pun yang punya partisi SPIFFS/LittleFS ≥ 1MB).
4. Tekan **Upload**.

### 4.2 Upload Web Assets ke LittleFS

1. Setelah plugin LittleFS Upload terpasang:
2. Tekan **Ctrl+Shift+P** → ketik "Upload LittleFS" → pilih.
3. Folder `data/` akan ter-flash ke partisi LittleFS.

### 4.3 Ganti SSID WiFi (opsional)

Di awal `esp32Firmware.ino`:
```cpp
const char* WIFI_SSID = "Biznet";
const char* WIFI_PASS = "12345678";
```

---

## 5. Cara Pakai

### 5.1 Boot Screen

Saat menyala, TFT menampilkan checklist boot:
```
[ OK ]  GPIO Init
[ OK ]  ADC Init
[ OK ]  TFT + Touch
[ OK ]  LittleFS
[ OK ]  Settings        3 idx
[ OK ]  WiFi            192.168.x.x
[ OK ]  mDNS            bayu.local
[ OK ]  Web Server      port 80

      SYSTEM READY
```

### 5.2 Akses Web

Buka di browser:
- `http://<IP yang muncul di TFT>` (mis. `http://192.168.1.20`)
- atau `http://bayu.local` (jika Bonjour/Avahi tersedia di OS klien)

### 5.3 Flow Pengujian

```
   ┌───────┐ START ┌─────────────┐ pilih ┌──────────────┐ konfirm ┌─────────┐
   │ IDLE  │ ────▶ │ SELECT_IDX  │ ───▶  │ WAIT_VALVE   │ ──────▶│ INHALE  │
   └───────┘       └─────────────┘       └──────────────┘         └────┬────┘
       ▲                                                                │
       │ BACK / Selesai                                                 ▼
       │                                                          ┌─────────────┐
   ┌───────┐  long beep   ┌─────────┐  ◄──────────────────────────│ PREPROCESS  │
   │RESULT │ ◄─────────── │SAMPLING │                             └─────┬───────┘
   └───────┘              └─────────┘ ◄────────────────────────────────┘
```

| State | Motor | TFT | Buzzer |
|---|---|---|---|
| IDLE | OFF (atau ON via Flush) | menampilkan HC/CO live + tombol START/FLUSH | senyap |
| SELECT_IDX | OFF | list index dengan tombol ▲ ▼ BACK | senyap |
| WAIT_VALVE | OFF | peringatan "katup tertutup" + BATAL/KONFIRMASI | senyap |
| INHALE | ON | state, HC/CO, chart, progress, STOP | 3 bip cepat (transisi) lalu 2 bip/detik |
| PREPROCESS | OFF | sama, motor mati untuk endapan udara | 2 bip/detik |
| SAMPLING | OFF | counter sample N/total + chart | 2 bip/detik |
| RESULT | OFF | hero warna sesuai hasil Rule Based AI + AVG | 1 bip panjang |

Tap **layar TFT** atau klik **web** — keduanya sinkron. Pilih index dari TFT akan otomatis menutup modal di web, dst.

### 5.4 Tab Settings (web)

- **Timing**: durasi inhale, preprocess, sample interval, sample count
- **Kalibrasi R0**: nilai R0 MQ-2 dan MQ-7 dalam Ohm (lihat §6 untuk kalibrasi)
- **Index Tahun Motor**: daftar tahun motor dengan threshold HC (ppm) dan CO (%); bisa add / edit / hapus

Klik **Simpan Settings** → semua tersimpan di NVS, bertahan setelah reboot.

### 5.5 Tab Log (Data Logger)

Setiap hasil pengujian (state RESULT) otomatis **dicatat ke SD card** sebagai 1 baris JSON di file `/logs.jsonl`. Setiap entri berisi:

- `ts`: timestamp (NTP, format ISO 8601 WIB)
- `idx_label`, `th_hc`, `th_co`: index motor & threshold yang dipakai
- `avg_hc`, `avg_co`, `label`, `code`: ringkasan hasil
- `s_hc[]`, `s_co[]`: array nilai sample HC/CO selama tahap SAMPLING (untuk replay grafik)

Di tab Log, web menampilkan:
- **Tabel riwayat** (terbaru di atas) — kolom: ID, Waktu, Index, AVG HC, AVG CO, Hasil
- Tombol **Refresh**, **Download** (file `.jsonl` mentah untuk backup), **Hapus Semua**
- Klik **Detail** → modal dengan grafik per-sample HC & CO (Chart.js)

Saat siklus pengujian selesai dan tab Log dibuka, tabel otomatis refresh setelah ~700ms (memberi waktu firmware menulis ke SD).

### 5.6 Tab Info

Menampilkan:
- Identitas skripsi (logo Polinema + judul + nama mahasiswa)
- RSSI Wi-Fi dengan 4-bar signal strength
- IP, uptime, free heap
- Status MQ-2 / MQ-7 (OK / DISCONNECTED dari heuristik ADC)
- Status motor (ACTIVE / IDLE), TFT (OK), nilai HC/CO real-time

---

## 6. Kalibrasi Sensor

### 6.1 Apa itu R0?

`R0` = resistansi sensor MQ saat ada di **udara bersih** (clean air baseline). Curve datasheet `ppm = a × (Rs/R0)^b` butuh nilai R0 yang benar untuk konversi ppm yang akurat.

### 6.2 Prosedur Kalibrasi

1. Tempatkan sensor di **udara bersih** (luar ruangan, jauh dari sumber gas).
2. Beri waktu warm-up:
   - MQ-2: ~24 jam preheat pertama, lalu ≥3 menit tiap pemakaian
   - MQ-7: ~48 jam preheat pertama, idealnya dengan siklus heater 5V/1.4V
3. Buka tab Info → catat nilai **ADC MQ-2** dan **ADC MQ-7**.
4. Hitung Rs di udara bersih:
   ```
   Vadc    = ADC × 3.3 / 4095
   Vmodule = Vadc × (4.7k + 10k) / 10k       ≈ Vadc × 1.47
   Rs      = 10k × (5 - Vmodule) / Vmodule
   ```
5. R0 ≈ Rs × faktor clean-air dari datasheet:
   - **MQ-2** clean air: Rs/R0 ≈ 9.83 → **R0 = Rs / 9.83**
   - **MQ-7** clean air: Rs/R0 ≈ 27.5 → **R0 = Rs / 27.5**
6. Masukkan nilai R0 di tab Settings → Simpan.

### 6.3 Kalibrasi Otomatis Target Idle (dinamis)

Alternatif tanpa hitung manual: tab **Settings → Kalibrasi Otomatis Target Idle**.

1. Nyalakan mesin sampai **idle stabil**, sensor sudah preheat ≥5 menit.
2. Isi **Target HC (ppm)** dan **Target CO (%)** — nilai pembacaan yang diinginkan untuk kondisi idle saat ini (default HC=75 ppm, CO=0.29%).
3. Tekan **Mulai Kalibrasi**. ESP32 merata-rata Rs selama ~10 detik (50 sampel), lalu back-solve R0 dari curve datasheet:
   ```
   R0 = Rs × (target_ppm / a)^(-1/b)
   ```
4. R0 baru otomatis tersimpan ke NVS dan field R0 di Settings ikut terisi.

API:
- `POST /api/calibrate?hc=<ppm>&co=<persen>` — **memicu start** kalibrasi (hanya di state IDLE; validasi HC 1–50000 ppm, CO 0.01–10 %; tanpa parameter memakai default firmware). Respon instan `{"ok":true,"started":true,"duration_ms":10000}`.
- Hasil dikirim via WebSocket frame `{"type":"calib_done", ...}`.
- `GET /api/calibrate` — status & hasil terakhir (fallback poll jika frame WS ter-drop).

> **Catatan desain (penting):** sampling kalibrasi dijalankan **non-blocking di `loop()`** (`calibTick()`), BUKAN di handler HTTP. Handler HTTP jalan di task `async_tcp` yang diawasi task watchdog (timeout 5 detik) — kalibrasi blocking 10 detik di sana memicu `task_wdt: async_tcp` → abort → ESP32 restart.

### 6.4 Filter Kalman pada Pembacaan Sensor

Pipeline pembacaan tiap 200ms:

```
ADC (avg 16 sampel) → Filter Kalman 1D → Vadc → Vmodule → Rs → ppm/%
```

- Filter Kalman scalar (`q=1`, `r=30`) diterapkan pada nilai ADC **sebelum** konversi ke tegangan/Rs/ppm — menghaluskan noise pembacaan dengan lag efektif ~5 sampel (~1 detik).
- ADC **mentah** tetap dipakai untuk health check sensor (deteksi rail 4095 / disconnect), supaya deteksi fault tidak tertunda filter.
- Tuning di firmware: `KF_Q` (besar = respon cepat) dan `KF_R` (besar = smoothing kuat).

---

## 7. State Machine & Rule Based AI

### 7.1 Logika Rule Based AI (matriks 2×2)

|  | CO < th_co | CO ≥ th_co |
|---|---|---|
| **HC < th_hc** | `NORMAL` (kode 0, hijau) | `GEJALA AWAL` (1, amber) |
| **HC ≥ th_hc** | `MISFIRE / PEMBAKARAN TAK SEMPURNA` (2, orange) | `PEMBAKARAN TIDAK NORMAL` (3, merah) |

Threshold diambil dari **index yang dipilih user** (dinamis di Settings).

### 7.2 Output Hasil

- AVG HC = rata-rata N sampel HC selama tahap SAMPLING
- AVG CO = rata-rata N sampel CO selama tahap SAMPLING
- Label Rule Based AI
- Warna kategori di TFT dan card di web

---

## 8. API Reference

### 8.1 HTTP

| Endpoint | Method | Body / Response |
|---|---|---|
| `/` | GET | `index.html` dari LittleFS |
| `/api/settings` | GET | JSON konfigurasi lengkap |
| `/api/settings` | POST | JSON (sebagian field OK) → simpan |
| `/api/status` | GET | snapshot state + sensor + sistem (termasuk `sd_ok`) |
| `/api/logs` | GET | array ringkasan log (untuk tabel) |
| `/api/log?id=N` | GET | detail satu log (termasuk array sample) |
| `/api/logs` | DELETE | hapus semua log |
| `/api/logs/download` | GET | unduh file JSONL mentah (backup) |

#### Contoh `/api/settings`:
```json
{
  "inhale_ms": 10000,
  "preprocess_ms": 5000,
  "sample_interval_ms": 1000,
  "sample_count": 10,
  "r0_mq2": 10000,
  "r0_mq7": 10000,
  "indices": [
    { "label": "Motor < 2010",     "th_hc": 800, "th_co": 3.0 },
    { "label": "Motor 2010-2015",  "th_hc": 500, "th_co": 2.0 },
    { "label": "Motor > 2015",     "th_hc": 300, "th_co": 1.0 }
  ]
}
```

### 8.2 WebSocket (`/ws`)

#### Pesan dari Server → Client

| Type | Fields |
|---|---|
| `data` | `hc, co, state, motor, elapsed, phase_total, sampled, sample_target` (5 Hz) |
| `state` | `state, phase_total, selected_index, motor, flush` (saat transisi) |
| `result` | `label, code, avg_hc, avg_co, th_hc, th_co, index_label` |

#### Pesan dari Client → Server

| Command | Payload | Efek |
|---|---|---|
| `request_start` | — | IDLE → SELECT_IDX |
| `select_idx` | `index: <n>` | SELECT_IDX → WAIT_VALVE |
| `confirm_valve` | — | WAIT_VALVE → INHALE |
| `stop` | — | (any) → IDLE |
| `flush` | `on: bool` | hidup/matikan motor saat IDLE |
| `start` (legacy) | `index: <n>` | langsung IDLE → WAIT_VALVE |

---

## 9. Arsitektur Software

### 9.0 Dual-Core Task Layout

ESP32 punya 2 core; firmware membagi kerja supaya **tidak ada task yang bisa
memblokir task lain** (akar dari semua bug restart/macet sebelumnya):

```
CORE 1 — loopTask (realtime, tidak pernah blok):
  sensor ADC → Kalman → state machine → buzzer → TFT + touch → broadcast WS
  → kalibrasi non-blocking. TIDAK PERNAH menyentuh SD card.

CORE 0 — sdTask (worker I/O, satu-satunya penulis SD):
  antrian job (FreeRTOS queue): SDJOB_SAVE (simpan log run), SDJOB_REBUILD
  (bangun ulang cache daftar log). Hasil simpan dilaporkan balik ke loopTask
  (frame WS "log_saved") — kartu hasil web menampilkan ✓/✗.

async_tcp (HTTP + WebSocket, task watchdog 5 detik):
  tidak pernah I/O lama. GET /api/logs dilayani CACHE RAM; operasi ringan
  (detail/edit/hapus) inline dengan ioLock timeout + batas waktu baca 3 detik.
```

Sinkronisasi: **satu mutex `ioMutex`** (bus SPI TFT/touch/SD + `logsCache`),
tidak pernah nested → bebas deadlock. TFT/touch memakai **try-lock 25ms**:
kalau sdTask sedang menulis, frame TFT di-skip dan dicoba lagi ~20ms kemudian —
state machine, sensor, dan web tetap berjalan normal.

### 9.1 Non-Blocking Loop

```cpp
void loop() {
  // 1) baca sensor tiap 200ms (ADC — tanpa lock)
  if (now - lastSensorRead > 200) { readSensors(); chartPush(...); }
  // 2) kalibrasi non-blocking (no-op jika tidak aktif)
  calibTick();
  // 3) cek touch (try-lock SPI; skip jika sdTask sedang pakai bus)
  if (ioLock(25)) { if (getTouch(&tx, &ty)) dispatchTouch(tx, ty); ioUnlock(); }
  // 4) advance state machine (RESULT -> enqueue job simpan ke sdTask)
  tickStateMachine();
  // 5) update buzzer (millis-based)
  handleBuzzer();
  // 6) refresh TFT (partial update, try-lock)
  if (ioLock(25)) { tftRefresh(); ioUnlock(); }
  // 7) lapor hasil simpan log dari sdTask (frame WS "log_saved")
  if (logSaveNotify) { ... wsSend(...); }
  // 8) broadcast WS tiap 200ms
  if (now - lastBroadcast > 200) broadcastData();
  ws.cleanupClients();
}
```

Tidak ada `delay()` di loop. Semua timing pakai `millis()`.

### 9.2 Pembagian Tugas

| File | Tanggung Jawab |
|---|---|
| `esp32Firmware.ino` | State machine, sensor reading, TFT/Touch, WebServer/WS, Buzzer, Settings (NVS) |
| `data/index.html` | Markup 3 tab + modal + thesis card |
| `data/app.js` | WebSocket client, Chart.js, fetch settings, sync UI dengan state server |

### 9.3 Sinkronisasi TFT ↔ Web

```
        TFT touch ──┐
                    ├──▶ enterState() ──▶ broadcastState() ──▶ WebSocket ──▶ Web UI
        Web cmd  ──┘                            │
                                                ▼
                                            TFT redraw via tftRefresh()
```

Semua sumber input akhirnya mengubah `state` global. State broadcaster mengirim state baru ke semua client web. TFT loop mendeteksi perubahan state dan redraw layar. Tidak ada duplikasi state — single source of truth.

**Robust terhadap drop-frame (anti "web stuck di IDLE"):**
ESPAsyncWebServer punya antrian per-client (`WS_MAX_QUEUED_MESSAGES`, default 32 di ESP32). Saat antrian penuh / WiFi lag, frame `state` yang dikirim **sekali** per transisi bisa ter-drop diam-diam → dulu web bisa "tertinggal" sementara ESP32/TFT sudah lanjut. Solusi:

1. Pesan `data` periodik (200ms) kini membawa **snapshot state lengkap** (`state`, `selected_index`, `flush`), bukan hanya angka sensor.
2. Sisi web menyatukan semua logika sinkron ke `applyServerState(m)` yang dipanggil oleh pesan `state` **dan** `data`. Jadi walau frame `state` ter-drop, frame `data` berikutnya (≤200ms) otomatis menyelaraskan ulang UI (termasuk buka/tutup modal).
3. `broadcastData()` di-skip saat `ws.availableForWriteAll()` false — frame periodik tidak menyumbat antrian sehingga frame kritis (`state`/`result`) tetap punya ruang dan heap tidak menumpuk.

---

## 10. Troubleshooting

| Gejala | Kemungkinan Penyebab | Solusi |
|---|---|---|
| TFT layar putih / hitam | Pin SPI salah / library tidak match | Cek wiring, pastikan rotation 1, library Adafruit_ILI9341 |
| TFT putih setelah disentuh | Clash transaksi SPI touch vs TFT (bus dibagi) + clock TFT terlalu tinggi | Sudah di-mitigasi: (1) touch tidak gambar langsung, (2) `TFT_SPI_HZ`=16MHz, (3) touch di-throttle `TOUCH_POLL_MS`=40ms + jeda settle, (4) flush pakai partial redraw. Jika MASIH putih: turunkan `TFT_SPI_HZ` ke `10000000` |
| Restart `task_wdt: async_tcp` | Handler HTTP blocking >5 detik (task watchdog): kalibrasi blocking, atau akses SD wedged dari handler (tab Log auto-refresh 3 detik bisa memicu restart loop) | Sudah di-fix dengan arsitektur dual-core (§9.0): kalibrasi non-blocking via `calibTick()`; semua tulis SD di `sdTask` core 0; `GET /api/logs` dari cache RAM; akses SD inline di handler diserialisasi `ioMutex` + batas waktu 3 detik. Jangan pernah `delay()` panjang atau I/O lambat di handler AsyncWebServer |
| System run macet di INHALE / TFT tidak respon | Operasi SD lambat (kartu wedged) dulu dikerjakan di `loop()` → state machine & touch ikut tertahan | Sudah di-fix: simpan log & rebuild cache pindah ke `sdTask` (core 0); `loop()` (core 1) tidak pernah menyentuh SD; TFT pakai try-lock (skip frame, tidak menunggu) |
| Touch tidak responsif | T_CS tidak tersambung / kalibrasi salah | Sambungkan T_CS ke GPIO 15. Kalibrasi ulang di `TOUCH_XMIN`..`TOUCH_YMAX` |
| Touch koordinat meleset | Default calibration tidak match TFT Anda | Print `p.x, p.y` mentah saat tap pojok layar, update define |
| Tombol ke-tekan berulang / hang saat ditahan | Tanpa edge-detection touch akan retrigger | Sudah di-fix: `getTouch()` pakai edge-detection (1 aksi per tekan, wajib lepas dulu). Atur `TOUCH_DEBOUNCE` / `TOUCH_PRESS_MIN` bila perlu |
| Tap kurang sensitif / harus ditekan keras | `TOUCH_PRESS_MIN` terlalu tinggi | Turunkan `TOUCH_PRESS_MIN` (mis. 250). Default 350 |
| MQ selalu DISCONNECTED | ADC mentok 4095 (pin short ke 3.3V / divider lepas) | Cek wiring divider R1/R2. Catatan: ADC 0V = idle valid (bukan disconnect) |
| ppm/% absurd | R0 default (10k) tidak akurat | Lakukan kalibrasi (§6) |
| WiFi gagal connect | SSID/pass salah | Edit `WIFI_SSID/WIFI_PASS` di kode |
| `bayu.local` tidak resolve | Tidak ada mDNS responder di OS | Install Bonjour (Windows) / Avahi (Linux) atau pakai IP langsung |
| Web `chart not defined` | CDN diblokir | Pastikan klien punya internet. Untuk offline, host Chart.js di LittleFS |
| Browser stuck di "disconnected" | Server crash / WiFi drop | Restart ESP32 |
| Compile error `TBtn was not declared` | Struct urutan tidak benar | Pastikan `struct TBtn` ada SEBELUM fungsi pertama di file |

---

## 10.1 Known Issue — White Screen TFT (ILI9341 + XPT2046 share SPI)

### Gejala
Layar TFT tiba-tiba **putih** setelah beberapa kali sentuhan, **tetapi touch & ESP32 tetap berfungsi** (tombol masih bereaksi, web tetap jalan). Bukan crash — murni korupsi tampilan.

### Akar Masalah (terdokumentasi)
ILI9341 (display) dan XPT2046 (touch) **berbagi bus SPI yang sama** (SCK/MOSI/MISO). Ini bug terkenal pada modul TFT touch murah:
- Saat touch dibaca, XPT2046 meng-clock bus di ~2 MHz. Pulsa SCK/MOSI ini ter-couple ke jalur CS display (kabel panjang / PCB murah).
- ILI9341 (write-only dari sisi kita) sesekali **salah men-latch** pulsa tersebut sebagai command → register display rusak → semua pixel putih.
- Makin tinggi clock SPI display, makin marjinal sinyalnya → makin sering korup.

### Mitigasi yang sudah diterapkan (mode share-bus)
| Mitigasi | Nilai | Define |
|---|---|---|
| Clock SPI TFT diturunkan | 10 MHz | `TFT_SPI_HZ` |
| Polling touch di-throttle | 30 ms | `TOUCH_POLL_MS` |
| Jeda settle touch→TFT | 50 µs | (hardcoded di loop) |
| Touch handler tak gambar langsung | — | flag `tftDirty` |
| Edge-detection (1 aksi/tekan) | — | `getTouch()` |
| Flush pakai partial redraw | — | `tftUpdateIdleValues()` |

Jika **masih** sesekali putih, turunkan lagi: `#define TFT_SPI_HZ 8000000` (atau `6000000`).

### Solusi Definitif (jika software belum cukup)
Pindahkan touch ke **bus SPI terpisah (HSPI)** dengan pin sendiri — pendekatan board *Cheap Yellow Display*. Ini menghilangkan kontensi bus 100%. Rewire:

```
T_CLK  → GPIO 25      (sebelumnya share GPIO 18)
T_DO   → GPIO 27      (MISO, sebelumnya share GPIO 19)
T_DIN  → GPIO 32      (MOSI, sebelumnya share GPIO 23)
T_CS   → GPIO 33      (sebelumnya GPIO 15)
```
lalu di firmware:
```cpp
SPIClass touchSPI(HSPI);
touchSPI.begin(25 /*sck*/, 27 /*miso*/, 32 /*mosi*/, 33 /*cs*/);
ts.begin(touchSPI);   // bukan ts.begin()
ts.setRotation(1);
```

### Referensi
- [XPT2046_Touchscreen #26 — not working with Adafruit_ILI9341](https://github.com/PaulStoffregen/XPT2046_Touchscreen/issues/26)
- [XPT2046_Touchscreen #42 — display white/blue-black with Adafruit lib](https://github.com/PaulStoffregen/XPT2046_Touchscreen/issues/42)
- [ControllersTech — ILI9341 + XPT2046 shared SPI tutorial](https://controllerstech.com/ili9341-arduino-touchscreen-tutorial/)
- [Bodmer/TFT_eSPI #950 — White Screen ESP32 + ILI9341](https://github.com/Bodmer/TFT_eSPI/discussions/950)
- [Adafruit forum — ILI9341 turns white](https://forums.adafruit.com/viewtopic.php?t=214410)

---

## 10.2 Known Issue — SD Card FAIL di Shared SPI

### Gejala
Serial log menampilkan `SD FAIL` saat boot meskipun kartu terpasang.

### Akar Masalah
Bus SPI dibagi dengan TFT ILI9341 + touch XPT2046. Saat `SD.begin()` dipanggil, jika `TFT_CS` / `TOUCH_CS` belum di-deselect (HIGH), kedua chip akan ikut merespon clock SD dan merusak signaling. Selain itu beberapa kartu SD low-end gagal init pada 4 MHz di shared bus dan butuh fallback ke 1 MHz atau 400 kHz.

### Mitigasi yang Diterapkan
- Drive semua CS (TFT_CS=5, TOUCH_CS=15, SD_CS=26) ke HIGH **sebelum** `SPI.begin()`.
- Saat `SD.begin()`, deselect ulang TFT_CS & TOUCH_CS, beri delay 10 ms.
- Retry SD.begin di 3 frekuensi: **1 MHz → 4 MHz → 400 kHz**, ambil yang pertama sukses.

### Jika Masih Gagal (cek hardware)
- Kartu harus format **FAT32** (bukan exFAT), kapasitas ≤ 32 GB.
- Cek modul TFT: beberapa varian punya pin CS SD tidak terhubung dari pabrik, perlu jumper/solder ke header `SD_CS`.
- Pastikan VCC modul TFT stabil 3.3V — drop tegangan saat motor relay aktif bisa memutus SD.
- Beberapa modul TFT punya level shifter terpisah untuk SD; jika rusak SD tidak akan init.

---

## 11. Lampiran Tugas Akhir (Comment-Stripped)

Folder terpisah berisi versi source code tanpa komentar untuk lampiran skripsi:

```
../bayu_knalpotSystem_lampiran/esp32Firmware/
├── esp32Firmware.ino   (komentar dihapus, string literal dipertahankan)
└── data/
    ├── index.html
    └── app.js
```

Versi ini fungsional identik dengan source utama — hanya komentar `//`, `/* */`, dan `<!-- -->` yang dihilangkan agar listing lampiran lebih ringkas.

Skrip generator: `/tmp/strip_comments.py` (regex-based, aware terhadap string literal).

---

## 12. Lisensi & Atribusi

- ESP32 core: Espressif (Apache 2.0)
- Adafruit GFX/ILI9341: BSD
- XPT2046_Touchscreen: BSD
- Tailwind CSS via CDN, Chart.js v4 via CDN

Proyek ini untuk keperluan akademik (skripsi/proyek akhir). Logo Polinema adalah milik Politeknik Negeri Malang.

---

**© 2026 Anoki Bayu Defender — Politeknik Negeri Malang**
