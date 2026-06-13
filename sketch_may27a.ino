#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <M5GFX.h>
#include <M5PM1.h>
#include <M5Unified.h>
#include <M5UnitENV.h>
#include "default_image.h"

static constexpr uint8_t SD_SPI_CS_PIN = 47;
static constexpr uint8_t SD_SPI_SCK_PIN = 15;
static constexpr uint8_t SD_SPI_MOSI_PIN = 13;
static constexpr uint8_t SD_SPI_MISO_PIN = 14;
static constexpr uint8_t SHT40_MEASURE_HIGH_PRECISION = 0xFD;

static constexpr std::size_t MAX_IMAGE_FILES = 32;
static constexpr uint32_t SENSOR_UPDATE_INTERVAL_MS = 30UL * 60UL * 1000UL;
static constexpr int SWIPE_THRESHOLD_PX = 80;

M5PM1 pm1;

String imagePaths[MAX_IMAGE_FILES];
std::size_t imageCount = 0;
std::size_t currentImageIndex = 0;

bool powerReady = false;
bool sdReady = false;
bool sensorReady = false;
bool rtcReady = false;
bool hasSensorReading = false;

float temperatureC = 0.0f;
float humidityPct = 0.0f;
String lastUpdatedText = "NOT UPDATED";
int lastRtcSlot = -1;
uint32_t lastSensorUpdateMs = 0;
m5::I2C_Class* sensorBus = nullptr;
uint8_t sensorAddr = SHT40_I2C_ADDR_44;

struct ScreenLayout {
  int screenW;
  int screenH;
  int margin;
  int gap;
  int cardH;
  int cardY;
  int imageX;
  int imageY;
  int imageW;
  int imageH;
  int cardW;
  int tempCardX;
  int humCardX;
  int cardRadius;
  int sensorTopY;
  int sensorH;
};

ScreenLayout screenLayout {};
M5Canvas imageCanvas(&M5.Display);
M5Canvas sensorCanvas(&M5.Display);

static String normalizeRootPath(const char* path)
{
  String normalized = path == nullptr ? "" : String(path);
  if (!normalized.startsWith("/")) {
    normalized = "/" + normalized;
  }
  return normalized;
}

static bool isSupportedImagePath(const String& path)
{
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".bmp");
}

static bool beginPowerControl()
{
  const m5pm1_err_t err = pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
  if (err != M5PM1_OK) {
    Serial.println("PM1 init failed.");
    return false;
  }

  pm1.setLdoEnable(true);
  pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_3, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_3, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);
  return true;
}

static bool beginSdCard()
{
  if (!powerReady) {
    Serial.println("Skipping SD init because PM1 is unavailable.");
    return false;
  }

  if (pm1.digitalRead(M5PM1_GPIO_NUM_1) != LOW) {
    Serial.println("SD card not inserted.");
    return false;
  }

  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    Serial.println("SD init failed.");
    return false;
  }

  return true;
}

static void loadImageList()
{
  imageCount = 0;

  if (!sdReady) {
    return;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open SD root.");
    return;
  }

  File entry = root.openNextFile();
  while (entry && imageCount < MAX_IMAGE_FILES) {
    if (!entry.isDirectory()) {
      const String path = normalizeRootPath(entry.name());
      if (isSupportedImagePath(path)) {
        imagePaths[imageCount++] = path;
        Serial.printf("Found image: %s\r\n", path.c_str());
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  if (imageCount == 0) {
    Serial.println("No supported image files found in SD root.");
  }
}

static bool drawCurrentImage(lgfx::LGFXBase& gfx, int x, int y, int width, int height)
{
  if (sdReady && imageCount != 0) {
    const String& path = imagePaths[currentImageIndex];
    String lower = path;
    lower.toLowerCase();

    if (lower.endsWith(".png")) {
      return gfx.drawPngFile(SD, path.c_str(), x, y, width, height, 0, 0, 0.0f, 0.0f, middle_center);
    } else if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
      return gfx.drawJpgFile(SD, path.c_str(), x, y, width, height, 0, 0, 0.0f, 0.0f, middle_center);
    } else if (lower.endsWith(".bmp")) {
      return gfx.drawBmpFile(SD, path.c_str(), x, y, width, height, 0, 0, 0.0f, 0.0f, middle_center);
    }
  }

  return gfx.drawPng(default_image_png, default_image_png_len, x, y, width, height, 0, 0, 0.0f, 0.0f, middle_center);
}

static void updateLastUpdatedText()
{
  if (rtcReady) {
    const auto now = M5.Rtc.getDateTime();
    char text[32];
    snprintf(text, sizeof(text), "%02u/%02u %02u:%02u", static_cast<unsigned>(now.date.month),
             static_cast<unsigned>(now.date.date), static_cast<unsigned>(now.time.hours),
             static_cast<unsigned>(now.time.minutes));
    lastUpdatedText = text;
  } else {
    const uint32_t totalSeconds = millis() / 1000UL;
    const uint32_t hours = totalSeconds / 3600UL;
    const uint32_t minutes = (totalSeconds / 60UL) % 60UL;
    const uint32_t seconds = totalSeconds % 60UL;
    char text[32];
    snprintf(text, sizeof(text), "UPTIME %02lu:%02lu:%02lu", static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
    lastUpdatedText = text;
  }
}

static int readBatteryLevelPercent()
{
  const int level = static_cast<int>(M5.Power.getBatteryLevel());
  if (level < 0) {
    return -1;
  }
  return min(max(level, 0), 100);
}

static bool updateSensorReading()
{
  if (!sensorReady || sensorBus == nullptr) {
    return false;
  }

  if (!(sensorBus->start(sensorAddr, false, 400000U) && sensorBus->write(SHT40_MEASURE_HIGH_PRECISION) && sensorBus->stop())) {
    Serial.println("SHT40 write failed.");
    return false;
  }

  delay(10);

  uint8_t readbuffer[6];
  if (!(sensorBus->start(sensorAddr, true, 400000U) && sensorBus->read(readbuffer, sizeof(readbuffer), true) &&
        sensorBus->stop())) {
    Serial.println("SHT40 read failed.");
    return false;
  }

  auto crc8 = [](const uint8_t* data, size_t len) -> uint8_t {
    uint8_t crc = 0xFF;
    while (len--) {
      crc ^= *data++;
      for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
      }
    }
    return crc;
  };

  if (readbuffer[2] != crc8(readbuffer, 2) || readbuffer[5] != crc8(readbuffer + 3, 2)) {
    Serial.println("SHT40 update failed.");
    return false;
  }

  const float t_ticks = static_cast<uint16_t>(readbuffer[0] << 8 | readbuffer[1]);
  const float rh_ticks = static_cast<uint16_t>(readbuffer[3] << 8 | readbuffer[4]);
  temperatureC = -45.0f + (175.0f * t_ticks / 65535.0f);
  humidityPct = -6.0f + (125.0f * rh_ticks / 65535.0f);
  humidityPct = min(max(humidityPct, 0.0f), 100.0f);
  hasSensorReading = true;
  updateLastUpdatedText();
  Serial.printf("Sensor updated: %.1f C / %.1f %%\r\n", temperatureC, humidityPct);
  return true;
}

static bool beginSensor()
{
  struct SensorBusCandidate {
    m5::I2C_Class* bus;
    uint8_t addr;
    const char* label;
  };

  static SensorBusCandidate candidates[] = {
      {&M5.In_I2C, SHT40_I2C_ADDR_44, "In_I2C GPIO2/3 addr 0x44"},
      {&M5.In_I2C, SHT40_I2C_ADDR_45, "In_I2C GPIO2/3 addr 0x45"},
      {&M5.Ex_I2C, SHT40_I2C_ADDR_44, "Ex_I2C GPIO5/4 addr 0x44"},
      {&M5.Ex_I2C, SHT40_I2C_ADDR_45, "Ex_I2C GPIO5/4 addr 0x45"},
  };

  for (const auto& candidate : candidates) {
    if (candidate.bus->scanID(candidate.addr, 400000U)) {
      sensorBus = candidate.bus;
      sensorAddr = candidate.addr;
      Serial.printf("SHT40 found on %s\r\n", candidate.label);
      return true;
    }
  }

  Serial.println("SHT40 not found on any expected I2C bus.");
  return false;
}

static bool updateSensorReadingWithRetry(uint8_t attempts, uint32_t retryDelayMs)
{
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    if (updateSensorReading()) {
      return true;
    }
    if (attempt + 1 < attempts) {
      delay(retryDelayMs);
    }
  }
  return false;
}

static void drawCenteredText(lgfx::LGFXBase& gfx, const String& text, int x, int y, const lgfx::IFont* font, uint16_t color,
                             textdatum_t datum)
{
  gfx.setFont(font);
  gfx.setTextColor(color);
  gfx.setTextDatum(datum);
  gfx.drawString(text, x, y);
}

static ScreenLayout getScreenLayout()
{
  ScreenLayout layout {};
  layout.screenW = M5.Display.width();
  layout.screenH = M5.Display.height();
  layout.margin = 18;
  layout.gap = 14;
  layout.cardH = 180;
  layout.cardY = layout.screenH - layout.margin - layout.cardH;
  layout.imageX = layout.margin;
  layout.imageY = layout.margin;
  layout.imageW = layout.screenW - (layout.margin * 2);
  layout.imageH = layout.cardY - layout.gap - layout.imageY;
  layout.cardW = (layout.screenW - (layout.margin * 2) - layout.gap) / 2;
  layout.tempCardX = layout.margin;
  layout.humCardX = layout.margin + layout.cardW + layout.gap;
  layout.cardRadius = 14;
  layout.sensorTopY = layout.cardY - layout.gap;
  layout.sensorH = layout.screenH - layout.sensorTopY;
  return layout;
}

static void renderImageSection()
{
  imageCanvas.fillSprite(WHITE);
  imageCanvas.drawRoundRect(0, 0, screenLayout.imageW, screenLayout.imageH, 10, BLACK);

  if (!drawCurrentImage(imageCanvas, 6, 6, screenLayout.imageW - 12, screenLayout.imageH - 12)) {
    drawCenteredText(imageCanvas, "No image available", screenLayout.imageW / 2, (screenLayout.imageH / 2) - 14,
                     &fonts::FreeSansBold18pt7b, RED, middle_center);
    drawCenteredText(imageCanvas, "Embedded default image failed", screenLayout.imageW / 2, (screenLayout.imageH / 2) + 20,
                     &fonts::Font4, BLACK, middle_center);
  }

  imageCanvas.pushSprite(screenLayout.imageX, screenLayout.imageY);
}

static void renderSensorSection()
{
  const int cardLocalY = screenLayout.cardY - screenLayout.sensorTopY;
  const int swipeLocalY = (screenLayout.cardY - 6) - screenLayout.sensorTopY;
  const int metaLocalY = cardLocalY + screenLayout.cardH - 22;

  sensorCanvas.fillSprite(WHITE);
  drawCenteredText(sensorCanvas, "Swipe left/right to change the image", screenLayout.screenW / 2, swipeLocalY, &fonts::Font2, BLACK,
                   bottom_center);
  sensorCanvas.fillRoundRect(screenLayout.tempCardX, cardLocalY, screenLayout.cardW, screenLayout.cardH, screenLayout.cardRadius, WHITE);
  sensorCanvas.fillRoundRect(screenLayout.humCardX, cardLocalY, screenLayout.cardW, screenLayout.cardH, screenLayout.cardRadius, WHITE);
  sensorCanvas.drawRoundRect(screenLayout.tempCardX, cardLocalY, screenLayout.cardW, screenLayout.cardH, screenLayout.cardRadius, RED);
  sensorCanvas.drawRoundRect(screenLayout.humCardX, cardLocalY, screenLayout.cardW, screenLayout.cardH, screenLayout.cardRadius, BLUE);

  drawCenteredText(sensorCanvas, "TEMPERATURE", screenLayout.tempCardX + (screenLayout.cardW / 2), cardLocalY + 28,
                   &fonts::FreeSansBold12pt7b, RED, top_center);
  drawCenteredText(sensorCanvas, "HUMIDITY", screenLayout.humCardX + (screenLayout.cardW / 2), cardLocalY + 28,
                   &fonts::FreeSansBold12pt7b, BLUE, top_center);

  if (hasSensorReading) {
    char tempText[24];
    char humText[24];
    snprintf(tempText, sizeof(tempText), "%.1f C", temperatureC);
    snprintf(humText, sizeof(humText), "%.1f %%", humidityPct);

    drawCenteredText(sensorCanvas, tempText, screenLayout.tempCardX + (screenLayout.cardW / 2), cardLocalY + 108,
                     &fonts::FreeSansBold24pt7b, BLACK, middle_center);
    drawCenteredText(sensorCanvas, humText, screenLayout.humCardX + (screenLayout.cardW / 2), cardLocalY + 108,
                     &fonts::FreeSansBold24pt7b, BLACK, middle_center);
  } else if (sensorReady) {
    drawCenteredText(sensorCanvas, "Updating...", screenLayout.tempCardX + (screenLayout.cardW / 2), cardLocalY + 108,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
    drawCenteredText(sensorCanvas, "Updating...", screenLayout.humCardX + (screenLayout.cardW / 2), cardLocalY + 108,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
  } else {
    drawCenteredText(sensorCanvas, "Sensor", screenLayout.tempCardX + (screenLayout.cardW / 2), cardLocalY + 92,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
    drawCenteredText(sensorCanvas, "not found", screenLayout.tempCardX + (screenLayout.cardW / 2), cardLocalY + 128,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
    drawCenteredText(sensorCanvas, "Sensor", screenLayout.humCardX + (screenLayout.cardW / 2), cardLocalY + 92,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
    drawCenteredText(sensorCanvas, "not found", screenLayout.humCardX + (screenLayout.cardW / 2), cardLocalY + 128,
                     &fonts::FreeSansBold18pt7b, BLACK, middle_center);
  }

  const int batteryLevel = readBatteryLevelPercent();
  char batteryText[24];
  char updatedText[48];
  if (batteryLevel >= 0) {
    snprintf(batteryText, sizeof(batteryText), "BAT %d%%", batteryLevel);
  } else {
    snprintf(batteryText, sizeof(batteryText), "BAT --%%");
  }
  snprintf(updatedText, sizeof(updatedText), "UPDATED %s", lastUpdatedText.c_str());

  drawCenteredText(sensorCanvas, batteryText, screenLayout.tempCardX + (screenLayout.cardW / 2), metaLocalY, &fonts::Font2, BLACK,
                   middle_center);
  drawCenteredText(sensorCanvas, updatedText, screenLayout.humCardX + (screenLayout.cardW / 2), metaLocalY, &fonts::Font2, BLACK,
                   middle_center);

  sensorCanvas.pushSprite(0, screenLayout.sensorTopY);
}

static void renderScreen()
{
  renderImageSection();
  renderSensorSection();
}

static void selectRelativeImage(int step)
{
  if (imageCount == 0) {
    return;
  }

  const int count = static_cast<int>(imageCount);
  const int current = static_cast<int>(currentImageIndex);
  currentImageIndex = static_cast<std::size_t>((current + step + count) % count);
  Serial.printf("Selected image: %s\r\n", imagePaths[currentImageIndex].c_str());
  renderImageSection();
}

static void handleSwipeSelection()
{
  const auto touch = M5.Touch.getDetail();
  if (!touch.wasReleased()) {
    return;
  }

  const int dx = touch.distanceX();
  const int dy = touch.distanceY();
  if (abs(dx) < SWIPE_THRESHOLD_PX || abs(dx) <= abs(dy)) {
    return;
  }

  if (dx < 0) {
    selectRelativeImage(1);
  } else {
    selectRelativeImage(-1);
  }
}

static void refreshSensorIfNeeded()
{
  bool shouldRefresh = false;

  if (rtcReady) {
    const auto now = M5.Rtc.getDateTime();
    const int slot = ((now.time.hours * 60) + now.time.minutes) / 30;
    if (!hasSensorReading || lastRtcSlot != slot) {
      lastRtcSlot = slot;
      shouldRefresh = true;
    }
  } else {
    const uint32_t nowMs = millis();
    if (!hasSensorReading || (nowMs - lastSensorUpdateMs) >= SENSOR_UPDATE_INTERVAL_MS) {
      lastSensorUpdateMs = nowMs;
      shouldRefresh = true;
    }
  }

  if (!shouldRefresh) {
    return;
  }

  if (!rtcReady) {
    lastSensorUpdateMs = millis();
  }
  if (updateSensorReadingWithRetry(hasSensorReading ? 1 : 5, 50)) {
    renderSensorSection();
  }
}

void setup()
{
  auto cfg = M5.config();
  cfg.clear_display = false;
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Ex_I2C.begin();

  M5.Display.setRotation(0);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  screenLayout = getScreenLayout();
  imageCanvas.createSprite(screenLayout.imageW, screenLayout.imageH);
  sensorCanvas.createSprite(screenLayout.screenW, screenLayout.sensorH);

  powerReady = beginPowerControl();
  sdReady = beginSdCard();
  loadImageList();

  sensorReady = beginSensor();

  rtcReady = M5.Rtc.isEnabled();
  if (!rtcReady) {
    Serial.println("RTC not available. Falling back to millis().");
  }

  if (rtcReady) {
    const auto now = M5.Rtc.getDateTime();
    lastRtcSlot = ((now.time.hours * 60) + now.time.minutes) / 30;
  }
  updateSensorReadingWithRetry(5, 50);
  renderScreen();
}

void loop()
{
  M5.update();
  handleSwipeSelection();
  refreshSensorIfNeeded();
  delay(50);
}
