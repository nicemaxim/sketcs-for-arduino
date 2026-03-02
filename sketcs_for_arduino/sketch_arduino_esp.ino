#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

#include <EEPROM.h>
#include <time.h>
#include "DHT.h"

// ---------- Wi-Fi / Telegram ----------
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"
#define BOT_TOKEN     "123456789:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

// Заполни chat_id, иначе бот не сможет сам слать тревоги/подтверждения
String ALLOWED_CHAT_ID = "123456789";

// Root cert for api.telegram.org (используется в примерах UniversalTelegramBot) [web:89]
X509List cert(TELEGRAM_CERTIFICATE_ROOT);
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ---------- DHT11 ----------
#define DHTPIN  2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ---------- EEPROM config ----------
#define EEPROM_SIZE 128
#define CFG_MAGIC 0x51A7C0DEu

struct Config {
  uint32_t magic;

  float tMin, tMax;
  float hMin, hMax;

  // Unix time (секунды с 1970). 0 = ещё не было.
  uint32_t lastDisinfection;
  uint32_t lastVentilation;
};

Config cfg;

// ---------- Интервалы ----------
const unsigned long BOT_MTBS = 1200;
unsigned long bot_lasttime = 0;

const unsigned long SENSOR_PERIOD = 2500;
unsigned long lastSensorRead = 0;

// ---------- Аварийные уведомления ----------
bool wasOutOfRange = false;

// ---------- UI state ----------
enum SettingStep : uint8_t { STEP_NONE = 0, STEP_PICK_PARAM, STEP_ENTER_VALUE, STEP_PICK_OPERATION };
enum ParamToSet : uint8_t { PARAM_NONE = 0, PARAM_TMIN, PARAM_TMAX, PARAM_HMIN, PARAM_HMAX };
enum OperationType : uint8_t { OP_NONE = 0, OP_DISINF, OP_VENT };

SettingStep step = STEP_NONE;
ParamToSet param = PARAM_NONE;
OperationType op = OP_NONE;

// ---------- Helpers ----------
bool isAllowedChat(const String& chat_id) {
  return (ALLOWED_CHAT_ID.length() == 0) ? true : (chat_id == ALLOWED_CHAT_ID);
}

bool toFloatSafe(const String& s, float &out) {
  String t = s;
  t.trim();
  t.replace(",", ".");
  char *endptr = nullptr;
  out = strtof(t.c_str(), &endptr);
  return endptr != t.c_str() && *endptr == '\0';
}

void setDefaultConfig() {
  cfg.magic = CFG_MAGIC;
  cfg.tMin = 5.0;  cfg.tMax = 25.0;
  cfg.hMin = 40.0; cfg.hMax = 70.0;
  cfg.lastDisinfection = 0;
  cfg.lastVentilation = 0;
}

void normalizeConfig() {
  if (cfg.tMin > cfg.tMax) { float tmp = cfg.tMin; cfg.tMin = cfg.tMax; cfg.tMax = tmp; }
  if (cfg.hMin > cfg.hMax) { float tmp = cfg.hMin; cfg.hMin = cfg.hMax; cfg.hMax = tmp; }
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);                 // чтение структуры из EEPROM [web:102]
  if (cfg.magic != CFG_MAGIC) {
    setDefaultConfig();
    EEPROM.put(0, cfg);
    EEPROM.commit();                  // запись во flash [web:102]
  }
  EEPROM.end();
}

void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, cfg);
  EEPROM.commit();                    // commit обязателен на ESP8266 [web:102]
  EEPROM.end();
}

uint32_t nowUnix() {
  time_t now = time(nullptr);
  if (now < 24 * 3600) return 0; // время не поднялось
  return (uint32_t)now;
}

String formatUnix(uint32_t ts) {
  if (ts == 0) return "Никогда (нет записи)";

  time_t t = (time_t)ts;
  struct tm *tm_info = localtime(&t);
  char buf[32];
  // YYYY-MM-DD HH:MM
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
           tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
           tm_info->tm_hour, tm_info->tm_min);
  return String(buf);
}

String limitsText() {
  String msg;
  msg += "Лимиты:\n";
  msg += "T: " + String(cfg.tMin, 1) + " .. " + String(cfg.tMax, 1) + " °C\n";
  msg += "H: " + String(cfg.hMin, 1) + " .. " + String(cfg.hMax, 1) + " %";
  return msg;
}

// ---------- Keyboards (ReplyKeyboardMarkup) ----------
String mainKeyboardJson() {
  return F("["
           "[\"Показатели\",\"Настроить лимиты\"],"
           "[\"Лимиты\",\"Последние операции\"]"
           "]");
}

String setupKeyboardJson() {
  return F("["
           "[\"T min\",\"T max\"],"
           "[\"H min\",\"H max\"],"
           "[\"Назад\"]"
           "]");
}

String cancelKeyboardJson() {
  return F("[[\"Отмена\"]]");
}

String operationsKeyboardJson() {
  return F("["
           "[\"Дезинфекция\",\"Проветривание\"],"
           "[\"Назад\"]"
           "]");
}

String operationActionKeyboardJson() {
  return F("["
           "[\"Отметить выполнено сейчас\"],"
           "[\"Назад\"]"
           "]");
}

void sendKeyboard(const String& chat_id, const String& text, const String& keyboardJson) {
  bot.sendMessageWithReplyKeyboard(chat_id, text, "", keyboardJson, true, false, false); // [web:80]
}

void sendMainMenu(const String& chat_id) {
  step = STEP_NONE;
  param = PARAM_NONE;
  op = OP_NONE;
  sendKeyboard(chat_id, "Меню:", mainKeyboardJson());
}

void sendSetupMenu(const String& chat_id) {
  step = STEP_PICK_PARAM;
  param = PARAM_NONE;
  sendKeyboard(chat_id, "Что настраиваем?", setupKeyboardJson());
}

void sendOperationsMenu(const String& chat_id) {
  step = STEP_PICK_OPERATION;
  op = OP_NONE;
  sendKeyboard(chat_id, "Выбери операцию:", operationsKeyboardJson());
}

String readSensorText(bool &ok, bool &outOfRange) {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    ok = false;
    outOfRange = false;
    return "Ошибка чтения DHT11 (NaN). Проверь питание/пин/датчик.";
  }

  ok = true;
  outOfRange = (t < cfg.tMin || t > cfg.tMax || h < cfg.hMin || h > cfg.hMax);

  String msg = "Склад сейчас:\n";
  msg += "T: " + String(t, 1) + " °C\n";
  msg += "H: " + String(h, 1) + " %\n";
  msg += outOfRange ? "Статус: ВНЕ лимитов" : "Статус: OK";
  return msg;
}

void askValueForParam(const String& chat_id) {
  step = STEP_ENTER_VALUE;

  String p;
  if (param == PARAM_TMIN) p = "T min (°C)";
  else if (param == PARAM_TMAX) p = "T max (°C)";
  else if (param == PARAM_HMIN) p = "H min (%)";
  else if (param == PARAM_HMAX) p = "H max (%)";
  else p = "параметр";

  sendKeyboard(chat_id, "Введи число для " + p + " (или Отмена):", cancelKeyboardJson());
}

void applyValueToParam(float v) {
  if (param == PARAM_TMIN) cfg.tMin = v;
  else if (param == PARAM_TMAX) cfg.tMax = v;
  else if (param == PARAM_HMIN) cfg.hMin = v;
  else if (param == PARAM_HMAX) cfg.hMax = v;

  normalizeConfig();
  saveConfig(); // сохраняем лимиты сразу [web:102]
}

String operationStatusText(OperationType which) {
  if (which == OP_DISINF) {
    return "Последняя дезинфекция: " + formatUnix(cfg.lastDisinfection);
  } else if (which == OP_VENT) {
    return "Последнее проветривание: " + formatUnix(cfg.lastVentilation);
  }
  return "Не выбрана операция.";
}

void markOperationNow(OperationType which) {
  uint32_t ts = nowUnix();
  if (ts == 0) return;

  if (which == OP_DISINF) cfg.lastDisinfection = ts;
  if (which == OP_VENT)   cfg.lastVentilation  = ts;

  saveConfig(); // сохраняем отметку операции [web:102]
}

// ---------- Telegram handler ----------
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text    = bot.messages[i].text;

    if (!isAllowedChat(chat_id)) {
      bot.sendMessage(chat_id, "Доступ запрещён.", "");
      continue;
    }

    if (text == "/start") { sendMainMenu(chat_id); continue; }
    if (text == "/id") { bot.sendMessage(chat_id, "Ваш chat_id: " + chat_id, ""); continue; }

    // --- Шаг: ввод числа для лимита ---
    if (step == STEP_ENTER_VALUE) {
      if (text == "Отмена") { sendSetupMenu(chat_id); continue; }

      float v;
      if (!toFloatSafe(text, v)) {
        sendKeyboard(chat_id, "Нужно число. Пример: 23.5 (или Отмена).", cancelKeyboardJson());
        continue;
      }

      applyValueToParam(v);
      bot.sendMessage(chat_id, "Сохранено.\n" + limitsText(), "");
      sendSetupMenu(chat_id);
      continue;
    }

    // --- Шаг: выбор параметра лимита ---
    if (step == STEP_PICK_PARAM) {
      if (text == "Назад") { sendMainMenu(chat_id); continue; }
      if (text == "T min") { param = PARAM_TMIN; askValueForParam(chat_id); continue; }
      if (text == "T max") { param = PARAM_TMAX; askValueForParam(chat_id); continue; }
      if (text == "H min") { param = PARAM_HMIN; askValueForParam(chat_id); continue; }
      if (text == "H max") { param = PARAM_HMAX; askValueForParam(chat_id); continue; }

      sendSetupMenu(chat_id);
      continue;
    }

    // --- Шаг: выбор операции (дезинфекция/проветривание) ---
    if (step == STEP_PICK_OPERATION) {
      if (text == "Назад") { sendMainMenu(chat_id); continue; }

      if (text == "Дезинфекция") {
        op = OP_DISINF;
        sendKeyboard(chat_id, operationStatusText(op) + "\n\nЧто сделать?", operationActionKeyboardJson());
        continue;
      }
      if (text == "Проветривание") {
        op = OP_VENT;
        sendKeyboard(chat_id, operationStatusText(op) + "\n\nЧто сделать?", operationActionKeyboardJson());
        continue;
      }

      sendOperationsMenu(chat_id);
      continue;
    }

    // --- Действия внутри выбранной операции ---
    if (op != OP_NONE) {
      if (text == "Назад") { sendOperationsMenu(chat_id); continue; }

      if (text == "Отметить выполнено сейчас") {
        uint32_t ts = nowUnix();
        if (ts == 0) {
          bot.sendMessage(chat_id, "Время ещё не синхронизировано (NTP). Подожди минуту и попробуй снова.", "");
          sendOperationsMenu(chat_id);
          continue;
        }
        markOperationNow(op);
        sendKeyboard(chat_id, operationStatusText(op) + "\n\nОтмечено.", operationActionKeyboardJson());
        continue;
      }

      // если прислали что-то странное — покажем меню операции
      sendKeyboard(chat_id, operationStatusText(op) + "\n\nЧто сделать?", operationActionKeyboardJson());
      continue;
    }

    // --- Главное меню ---
    if (text == "Показатели") {
      bool ok, outOfRange;
      bot.sendMessage(chat_id, readSensorText(ok, outOfRange), "");
      sendMainMenu(chat_id);
    } else if (text == "Лимиты") {
      bot.sendMessage(chat_id, limitsText(), "");
      sendMainMenu(chat_id);
    } else if (text == "Настроить лимиты") {
      sendSetupMenu(chat_id);
    } else if (text == "Последние операции") {
      sendOperationsMenu(chat_id);
    } else {
      sendMainMenu(chat_id);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  dht.begin();
  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  // TLS/время (нужно и для Telegram TLS, и для хранения времени операций) [web:89]
  configTime(0, 0, "pool.ntp.org");
  client.setTrustAnchors(&cert);

  time_t now = time(nullptr);
  while (now < 24 * 3600) { delay(200); now = time(nullptr); }
}

void loop() {
  // Telegram polling
  if (millis() - bot_lasttime > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    bot_lasttime = millis();
  }

  // Контроль лимитов + уведомление при выходе
  if (millis() - lastSensorRead > SENSOR_PERIOD) {
    lastSensorRead = millis();

    bool ok, outOfRange;
    String current = readSensorText(ok, outOfRange);

    if (ok) {
      if (outOfRange && !wasOutOfRange && ALLOWED_CHAT_ID.length() > 0) {
        bot.sendMessage(ALLOWED_CHAT_ID,
                        "ВНИМАНИЕ! Параметры вышли за лимиты.\n\n" + current,
                        "");
      }
      wasOutOfRange = outOfRange;
    }
  }
}