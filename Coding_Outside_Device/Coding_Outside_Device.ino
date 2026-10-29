// Outside Device (LED Sentiasa Menyala 24 Jam - Tanpa Pemadaman Waktu Malam)
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "time.h"

// 1. TETAPAN AP & FIREBASE
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define FIREBASE_SECRET "YOUR_FIREBASE_SECRET"
#define FIREBASE_HOST   "YOUR_FIREBASE_HOST"

// 2. PIN HARDWARE ESP32 & TETAPAN LED INDIVIDU (2 PIN STRIP)
#define PIN_POWER_LED    2                                  // Pin LED penanda peranti hidup (GPIO 2)
#define PIN_NEOPIXEL_1   4                                  // Strip 1: Kawal Meja 1 hingga 6
#define PIN_NEOPIXEL_2   18                                 // Strip 2: Kawal Meja 7 hingga 12

#define LED_PER_LECTURER 10                                 // 10 biji LED diperuntukkan untuk setiap pensyarah
#define NUM_LECTURERS    12                                 // Jumlah 12 meja pensyarah
#define LEDS_PER_STRIP   (LED_PER_LECTURER * 6)             // 60 LED per strip (6 meja x 10 LED)

// Inisialisasi 2 Objek NeoPixel Strip
Adafruit_NeoPixel strip1(LEDS_PER_STRIP, PIN_NEOPIXEL_1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip2(LEDS_PER_STRIP, PIN_NEOPIXEL_2, NEO_GRB + NEO_KHZ800);

// 3. PEMBOLEHUBAH GLOBAL & TIMERS
int ganjilGagalWiFi = 0;
unsigned long lastCheck = 0, lastHeartbeat = 0;

const unsigned long FIREBASE_INTERVAL  = 3000;  // Semak status setiap 3s
const unsigned long HEARTBEAT_INTERVAL = 60000; // Heartbeat 1 minit

// Prototaip Fungsi
void semakFirebase();
void hantarHeartbeat();
void seliaSelfHealingWiFi();
void setWarnaLEDMeja(int indexMeja, uint8_t r, uint8_t g, uint8_t b);
void kemaskiniWarnaLEDIndividu(int indexMeja, String status);

void setup() {
  Serial.begin(115200);

  // Tetapan Power LED
  pinMode(PIN_POWER_LED, OUTPUT);
  digitalWrite(PIN_POWER_LED, HIGH); // Nyalakan LED penanda peranti hidup

  // Inisialisasi kedua-dua LED Strip
  strip1.begin();
  strip1.setBrightness(50); // Kecerahan sederhana (0-255)
  strip1.show();

  strip2.begin();
  strip2.setBrightness(50);
  strip2.show();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Menyambung Wi-Fi...");
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.println("\n✅ Wi-Fi Terhubung!");

  // Konfigurasi Watchdog Timer untuk ESP32 Core v3.x
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 10000, // 10 saat
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL);

  hantarHeartbeat();
  Serial.println("--- OUTSIDE DEVICE (2 STRIPS + POWER LED) SIAP BEROPERASI (24 JAM) ---");
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  // 🛠 SELIA SELF-HEALING WI-FI SECARA SILENT
  seliaSelfHealingWiFi();

  if (WiFi.status() != WL_CONNECTED) return;

  // 1. Semak Firebase secara berkala untuk Status LED Setiap Meja
  if (now - lastCheck >= FIREBASE_INTERVAL) {
    semakFirebase();
    lastCheck = now;
  }

  // 2. Heartbeat status peranti utama di luar
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    hantarHeartbeat();
    lastHeartbeat = now;
  }
}

// ================= FUNGSI WARNA LED =================

void setWarnaLEDMeja(int indexMeja, uint8_t r, uint8_t g, uint8_t b) {
  if (indexMeja < 6) {
    // Meja 0 hingga 5 (Meja 1-6) dipaparkan pada Strip 1 (GPIO 5)
    int startLED = indexMeja * LED_PER_LECTURER;
    int endLED = startLED + LED_PER_LECTURER;
    for (int i = startLED; i < endLED; i++) {
      strip1.setPixelColor(i, strip1.Color(r, g, b));
    }
  } else {
    // Meja 6 hingga 11 (Meja 7-12) dipaparkan pada Strip 2 (GPIO 18)
    int startLED = (indexMeja - 6) * LED_PER_LECTURER;
    int endLED = startLED + LED_PER_LECTURER;
    for (int i = startLED; i < endLED; i++) {
      strip2.setPixelColor(i, strip2.Color(r, g, b));
    }
  }
}

void kemaskiniWarnaLEDIndividu(int indexMeja, String status) {
  // Semakan waktu bekerja dibuang supaya LED sentiasa menyala mengikut status
  status.trim(); 
  status.toUpperCase();

  if (status == "ADA") {
    setWarnaLEDMeja(indexMeja, 0, 255, 0);                          // 🟢 Hijau
  } else if (status == "SIBUK" || status == "DALAM KELAS" || status == "KELAS" || status == "MESYUARAT") {
    setWarnaLEDMeja(indexMeja, 255, 0, 0);                         // 🔴 Merah
  } else if (status == "PULANG" || status == "LUAR KAWASAN/KECEMASAN") {
    setWarnaLEDMeja(indexMeja, 0, 0, 255);                         // 🔵 Biru
  } else if (status == "CUTI SAKIT" || status == "CUTI") {
    setWarnaLEDMeja(indexMeja, 255, 255, 0);                       // 🟡 Kuning
  } else {
    setWarnaLEDMeja(indexMeja, 255, 255, 255);                     // ⚪ Putih (Status Lalai / Tidak Dikenali)
  }
}

// ================= FUNGSI SEMAK FIREBASE (MENGGUNAKAN MEJA_ID SPESIFIK) =================

void semakFirebase() {
  HTTPClient http;
  http.begin(String(FIREBASE_HOST) + "/pensyarah_devices.json?auth=" + FIREBASE_SECRET);
  
  if (http.GET() == HTTP_CODE_OK) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      // Baca tepat dari meja_01 hingga meja_12 untuk mengelakkan ralat susunan JSON
      for (int i = 0; i < NUM_LECTURERS; i++) {
        char mejaId[10];
        sprintf(mejaId, "meja_%02d", i + 1);

        if (doc.containsKey(mejaId)) {
          String status = doc[mejaId]["status_kehadiran"].as<String>();
          kemaskiniWarnaLEDIndividu(i, status);
        } else {
          setWarnaLEDMeja(i, 0, 0, 0); // Jika data meja tiada dalam Firebase, padamkan LED
        }
      }
      // Hantar arahan kemaskini ke kedua-dua LED strip
      strip1.show();
      strip2.show();
    }
  }
  http.end();
}

// ================= FUNGSI PENJAGAAN SAMBUNGAN & HEARTBEAT =================

void seliaSelfHealingWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    ganjilGagalWiFi++;
    WiFi.reconnect();

    if (ganjilGagalWiFi >= 5 && ganjilGagalWiFi < 10) {
      WiFi.disconnect(true);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    if (ganjilGagalWiFi >= 10) {
      delay(1000);
      ESP.restart();
    }
    delay(2000);
  } else {
    ganjilGagalWiFi = 0;
  }
}

void hantarHeartbeat() {
  // Kelipkan Power LED seketika semasa hantar heartbeat
  digitalWrite(PIN_POWER_LED, LOW);
  delay(100);
  digitalWrite(PIN_POWER_LED, HIGH);

  HTTPClient http;
  http.begin(String(FIREBASE_HOST) + "/outside_device_main_status.json?auth=" + FIREBASE_SECRET);
  http.addHeader("Content-Type", "application/json");
  http.PUT("{\"online\":true,\"uptime_sec\":" + String(millis() / 1000) + "}");
  http.end();
}
