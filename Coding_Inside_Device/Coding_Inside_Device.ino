#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <RTClib.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <time.h>
#include "esp_sleep.h"

// 1. TETAPAN PERANTI & RANGKAIAN
#define DEVICE_ID       "meja_01" //Tukar mengikut nama di Firebase
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define FIREBASE_HOST   "YOUR_FIRBASE_HOST"
#define FIREBASE_SECRET "YOUR_FIREBASE_SECRET"
#define BOT_TOKEN       "YOUR_BOT_TOKEN"
#define ALLOWED_CHAT_ID "YOUR_CHAT_ID"

#define PATH_BASE      "/pensyarah_devices/" DEVICE_ID
#define PATH_STATUS    PATH_BASE "/status_kehadiran"
#define PATH_DOORBELL  PATH_BASE "/Doorbell"
#define PATH_VOLTAN    PATH_BASE "/bateri_voltan"
#define PATH_PERATUS   PATH_BASE "/bateri_peratus"

// 2. HARDWARE
#define LED_PIN         13
#define NUM_LEDS        8
#define LED_BRIGHTNESS  40
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Adafruit_INA219 ina219;
RTC_DS3231 rtc;
Preferences pref;

bool statusINA219 = false, statusRTC = false;
uint32_t masaTamatCutiEpoch = 0;
bool modCutiBerpenghujung = false, peringatanCutiDihantar = false, amaranBateriTelahDihantar = false;

unsigned long lastTelegramCheck = 0, telegramInterval = 1000;
unsigned long lastDoorbellCheck = 0, doorbellInterval = 2000;
unsigned long lastBatteryCheck = 0;
const unsigned long BATTERY_CHECK_INTERVAL = 900000;

// ================================================================
// HELPER FIREBASE & UTILITI
// ================================================================
String sendFirebaseReq(String path, String method = "GET", String payload = "") {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String(FIREBASE_HOST) + path + ".json?auth=" + FIREBASE_SECRET;
  http.begin(client, url);
  int code = 0;
  if (method == "GET") code = http.GET();
  else { http.addHeader("Content-Type", "application/json"); code = http.PUT(payload); }
  
  String res = (code == HTTP_CODE_OK || code == 200) ? http.getString() : "";
  http.end();
  res.replace("\"", ""); res.trim();
  return res;
}

int kiraPeratusBateri(float v) {
  return constrain((int)((v - 3.2) / 1.0 * 100.0), 0, 100);
}

void setWarnaLED(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

bool adakahWaktuBekerja() {
  if (!statusRTC) return true;
  DateTime now = rtc.now();
  int h = now.dayOfTheWeek(), j = now.hour();
  return (h >= 1 && h <= 5 && j >= 8 && j < 17);
}

void kemaskiniWarnaLED(String status) {
  if (!adakahWaktuBekerja()) { setWarnaLED(0, 0, 0); return; }
  status.trim(); status.toUpperCase();
  if (status == "ADA") setWarnaLED(0, 255, 0);                 // Hijau
  else if (status == "SIBUK" || status == "KELAS") setWarnaLED(255, 0, 0); // Merah
  else if (status == "PULANG") setWarnaLED(0, 0, 255);         // Biru
  else if (status == "CUTI") setWarnaLED(255, 255, 0);         // Kuning
  else setWarnaLED(255, 255, 255);                            // Putih (Default)
}

bool kemaskiniStatusFirebase(String status) {
  if (sendFirebaseReq(PATH_STATUS, "PUT", "\"" + status + "\"") != "") {
    kemaskiniWarnaLED(status);
    return true;
  }
  return false;
}

void kemaskiniBateriKeFirebase() {
  if (WiFi.status() != WL_CONNECTED || !statusINA219) return;
  float v = ina219.getBusVoltage_V();
  int p = kiraPeratusBateri(v);
  sendFirebaseReq(PATH_VOLTAN, "PUT", String(v, 2));
  sendFirebaseReq(PATH_PERATUS, "PUT", String(p));
  Serial.printf("🔋 Bateri: %.2fV (%d%%)\n", v, p);
}

void semakAmaranBateriAutomatik() {
  if (!statusINA219) return;
  float v = ina219.getBusVoltage_V();
  int p = kiraPeratusBateri(v);
  if (p <= 20 && !amaranBateriTelahDihantar) {
    bot.sendMessage(ALLOWED_CHAT_ID, "⚠️ *AMARAN BATERI RENDAH! (" DEVICE_ID ")*\nBateri: *" + String(p) + "%* (" + String(v, 2) + "V). Sila cas peranti.", "Markdown");
    amaranBateriTelahDihantar = true;
  } else if (p > 25) amaranBateriTelahDihantar = false;
}

// ================================================================
// PENGURUSAN CUTI (NVRAM FLASH)
// ================================================================
void simpanCutiKeFlash(uint32_t epoch, bool aktif) {
  pref.begin("cuti_app", false);
  pref.putUInt("epoch_tamat", epoch);
  pref.putBool("is_aktif", aktif);
  pref.putBool("peringatan", false);
  pref.end();
  masaTamatCutiEpoch = epoch; modCutiBerpenghujung = aktif; peringatanCutiDihantar = false;
}

void muatCutiDariFlash() {
  pref.begin("cuti_app", true);
  masaTamatCutiEpoch = pref.getUInt("epoch_tamat", 0);
  modCutiBerpenghujung = pref.getBool("is_aktif", false);
  peringatanCutiDihantar = pref.getBool("peringatan", false);
  pref.end();
}

uint32_t tambahMasaAbaikanWeekend(uint32_t startEpoch, int hariBekerja) {
  uint32_t curr = startEpoch;
  for (int h = 0; h < hariBekerja; ) {
    curr += 86400UL;
    int day = DateTime(curr).dayOfTheWeek();
    if (day != 0 && day != 6) h++;
  }
  return curr;
}

void tetapkanCutiHari(int bilanganHari, bool tundaHujungMinggu) {
  if (!statusRTC) return;
  uint32_t nowEpoch = rtc.now().unixtime();
  uint32_t tamatEpoch = tundaHujungMinggu ? tambahMasaAbaikanWeekend(nowEpoch, bilanganHari) : nowEpoch + (bilanganHari * 86400UL);
  simpanCutiKeFlash(tamatEpoch, true);
  kemaskiniStatusFirebase("CUTI");
}

void semakPemasaCutiRTC() {
  if (!statusRTC || !modCutiBerpenghujung) return;
  uint32_t now = rtc.now().unixtime();
  if (!peringatanCutiDihantar && (masaTamatCutiEpoch - now <= 3600) && (masaTamatCutiEpoch > now)) {
    peringatanCutiDihantar = true;
    pref.begin("cuti_app", false); pref.putBool("peringatan", true); pref.end();
    bot.sendMessage(ALLOWED_CHAT_ID, "⚠️ *PERINGATAN CUTI:* Tempoh cuti anda akan tamat dalam *1 Jam lagi*.", "Markdown");
  }
  if (now >= masaTamatCutiEpoch) {
    simpanCutiKeFlash(0, false);
    if (kemaskiniStatusFirebase("ADA")) {
      bot.sendMessage(ALLOWED_CHAT_ID, "⏰ *TEMPOH CUTI TAMAT!*\nStatus di-reset kepada 🟢 *ADA*.", "Markdown");
    }
  }
}

// ================================================================
// TELEGRAM & SISTEM
// ================================================================
void hantarPapanKunciButang(String chat_id, String msg) {
  String kb = "[[\"🟢 ADA\", \"🔴 SIBUK\"], [\"🔵 PULANG\", \"🟡 CUTI\"], [\"📊 SEMAK STATUS\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "Markdown", kb, true, false, false);
}

void kendalikanMesejTelegram(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    if (chat_id != ALLOWED_CHAT_ID) { bot.sendMessage(chat_id, "⛔ *AKSES DITOLAK!*", "Markdown"); continue; }

    String st = "";
    if (text == "/ada" || text == "🟢 ADA") st = "ADA";
    else if (text == "/sibuk" || text == "/kelas" || text == "🔴 SIBUK") st = "SIBUK";
    else if (text == "/pulang" || text == "🔵 PULANG") st = "PULANG";
    else if (text == "/cuti" || text == "🟡 CUTI") st = "CUTI";

    if (st != "") {
      simpanCutiKeFlash(0, false);
      if (kemaskiniStatusFirebase(st)) bot.sendMessage(chat_id, "Status dikemas kini: *" + st + "*", "Markdown");
    }
    else if (text == "/start" || text == "/menu") {
      hantarPapanKunciButang(chat_id, "Pilih status anda:\n• `/cuti 2` (Set Cuti 2 Hari)\n• `/tambahcuti 1`\n• `/batalcuti`");
    }
    else if (text == "/batalcuti") {
      if (modCutiBerpenghujung) {
        simpanCutiKeFlash(0, false); kemaskiniStatusFirebase("ADA");
        bot.sendMessage(chat_id, "✅ Pemasa cuti *DIBATALKAN*. Status: 🟢 *ADA*.", "Markdown");
      } else bot.sendMessage(chat_id, "ℹ️ Tiada pemasa cuti aktif.", "Markdown");
    }
    else if (text.startsWith("/tambahcuti ")) {
      int h = text.substring(12).toInt();
      if (h > 0 && modCutiBerpenghujung && statusRTC) {
        uint32_t epochBaru = masaTamatCutiEpoch + (h * 86400UL);
        simpanCutiKeFlash(epochBaru, true);
        DateTime dt(epochBaru);
        char buf[30]; snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d", dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute());
        bot.sendMessage(chat_id, "➕ Cuti ditambah *" + String(h) + " Hari*.\nTarikh Tamat: *" + String(buf) + "*", "Markdown");
      } else bot.sendMessage(chat_id, "⚠️ Format salah atau tiada cuti aktif.", "Markdown");
    }
    else if (text.startsWith("/cuti ")) {
      int h = text.substring(6).toInt();
      if (h > 0) {
        tetapkanCutiHari(h, true);
        DateTime dt(masaTamatCutiEpoch);
        char buf[30]; snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d", dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute());
        bot.sendMessage(chat_id, "🟡 Status: *CUTI*\n📅 *Durasi:* " + String(h) + " Hari Bekerja\n⏰ *Tamat:* " + String(buf), "Markdown");
      } else bot.sendMessage(chat_id, "⚠️ Format salah. Contoh: `/cuti 2`", "Markdown");
    }
    else if (text == "/status" || text == "📊 SEMAK STATUS") {
      String info = "📊 *STATUS PERANTI (" DEVICE_ID ")*\n\nStatus: *" + sendFirebaseReq(PATH_STATUS) + "*\n";
      if (modCutiBerpenghujung && statusRTC) {
        uint32_t baki = masaTamatCutiEpoch - rtc.now().unixtime();
        info += "📅 Baki Cuti: *" + String(baki / 86400) + "d " + String((baki % 86400) / 3600) + "h " + String((baki % 3600) / 60) + "m*\n";
      }
      if (statusRTC) {
        DateTime now = rtc.now();
        char buf[30]; snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d", now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
        info += "Masa: " + String(buf) + "\n";
      }
      info += statusINA219 ? "Bateri: " + String(ina219.getBusVoltage_V(), 2) + "V (" + String(kiraPeratusBateri(ina219.getBusVoltage_V())) + "%)\n" : "Bateri: ❌ INA219 Disconnected\n";
      info += "Mod: " + String(adakahWaktuBekerja() ? "☀️ Waktu Bekerja" : "🌙 Luar Waktu Bekerja") + "\nRAM: " + String(ESP.getFreeHeap()) + " B";
      bot.sendMessage(chat_id, info, "Markdown");
    }
    else hantarPapanKunciButang(chat_id, "Arahan tidak dikenali. Guna butang atau `/cuti 2`.");
  }
}

void setup() {
  Serial.begin(115200); Wire.begin(21, 22);
  strip.begin(); strip.setBrightness(LED_BRIGHTNESS); strip.show();

  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  secured_client.setInsecure();
  sendFirebaseReq(PATH_DOORBELL, "PUT", "false");

  statusINA219 = ina219.begin();
  if (rtc.begin()) {
    statusRTC = true;
    configTime(8 * 3600, 0, "pool.ntp.org");
    struct tm t; if (getLocalTime(&t)) rtc.adjust(DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec));
  }

  muatCutiDariFlash();
  delay(1000);
  kemaskiniWarnaLED(sendFirebaseReq(PATH_STATUS));
  kemaskiniBateriKeFirebase();
  semakAmaranBateriAutomatik();
}

void loop() {
  unsigned long now = millis();

  // Semak & re-connect Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(); WiFi.reconnect();
    unsigned long s = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - s < 10000) delay(500);
  }

  // Pelarasan Selang Semakan Berdasarkan Bateri & Waktu Bekerja
  bool waktuKerja = adakahWaktuBekerja();
  
  if (statusINA219 && kiraPeratusBateri(ina219.getBusVoltage_V()) <= 20) {
    telegramInterval = 5000; doorbellInterval = 6000;
  } else if (!waktuKerja) {
    telegramInterval = 3000; doorbellInterval = 5000;
  } else {
    telegramInterval = 1000; doorbellInterval = 2000;
  }

  // Pengurusan Lampu LED berdasarkan waktu bekerja
  if (!waktuKerja) {
    setWarnaLED(0, 0, 0); // Matikan LED luar waktu bekerja
  }

  // 1. Telegram Polling
  if (now - lastTelegramCheck >= telegramInterval) {
    if (ESP.getFreeHeap() < 20000) ESP.restart();
    int msgs = bot.getUpdates(bot.last_message_received + 1);
    while (msgs) { kendalikanMesejTelegram(msgs); msgs = bot.getUpdates(bot.last_message_received + 1); }
    lastTelegramCheck = now;
  }

  // 2. Semak Cuti RTC
  semakPemasaCutiRTC();

  // 3. Semak Doorbell
  if (now - lastDoorbellCheck >= doorbellInterval) {
    if (sendFirebaseReq(PATH_DOORBELL) == "true") {
      String st = sendFirebaseReq(PATH_STATUS);
      if (st != "SIBUK" && st != "CUTI") {
        bot.sendMessage(ALLOWED_CHAT_ID, "🔔 *PANGGILAN PINTU! (" DEVICE_ID ")*\nPelawat menekan butang panggilan.", "Markdown");
      }
      sendFirebaseReq(PATH_DOORBELL, "PUT", "false");
    }
    lastDoorbellCheck = now;
  }

  // 4. Semak Bateri Berkala
  if (now - lastBatteryCheck >= BATTERY_CHECK_INTERVAL) {
    kemaskiniBateriKeFirebase(); semakAmaranBateriAutomatik();
    lastBatteryCheck = now;
  }

  // 5. Deep Sleep Sahaja (Apabila Bateri Sangat Critical ≤15%)
  if (statusINA219 && kiraPeratusBateri(ina219.getBusVoltage_V()) <= 15) {
    setWarnaLED(0, 0, 0);
    esp_sleep_enable_timer_wakeup(30ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
}