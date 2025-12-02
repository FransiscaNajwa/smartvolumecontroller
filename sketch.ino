#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// === PIN sesuai wiring Wokwi / ESP32-S3 ===
#define POT_PIN     3      // potensiometer (ADC)
#define LED_PIN     5      // LED indikator
#define BUZZER_PIN  13     // buzzer
#define BUTTON_PIN  15     // tombol mute/unmute
#define SDA_PIN     17     // OLED SDA
#define SCL_PIN     8     // OLED SCL

// FreeRTOS objects
QueueHandle_t volumeQueue;
SemaphoreHandle_t volumeMutex;

// Variabel global
int volumeValue = 0;           // terakhir dibaca dari pot (0-100)
volatile bool isMuted = false; // status mute

// ================= CORE 0 TASK (BACA SENSOR + TOMBOL) ===================
void TaskSensor(void *pv) {
  int lastButtonState = HIGH;
  int currentButtonState = HIGH;

  pinMode(POT_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  for (;;) {
    // --- Baca Potensiometer ---
    int raw = analogRead(POT_PIN);          // 0 - 4095
    int vol = map(raw, 0, 4095, 0, 100);    // 0 - 100 %

    // Simpan ke variabel global pakai mutex
    xSemaphoreTake(volumeMutex, portMAX_DELAY);
    volumeValue = vol;
    xSemaphoreGive(volumeMutex);

    // Kirim ke queue (non-blocking)
    xQueueSend(volumeQueue, &vol, 0);

    // --- Baca Tombol (toggle mute) ---
    lastButtonState = currentButtonState;
    currentButtonState = digitalRead(BUTTON_PIN);

    // Deteksi tekan (HIGH -> LOW)
    if (lastButtonState == HIGH && currentButtonState == LOW) {
      isMuted = !isMuted; // toggle status mute
      Serial.printf("[Core %d] Button pressed, MUTE = %s\n",
                    xPortGetCoreID(), isMuted ? "ON" : "OFF");
      vTaskDelay(200 / portTICK_PERIOD_MS); // debounce
    }

    vTaskDelay(50 / portTICK_PERIOD_MS); // sampling 20 Hz
  }
}

// ================= CORE 1 TASK (OUTPUT) ===================
void TaskOutput(void *pv) {
  int vol = 0;

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  for (;;) {
    // Tunggu data volume dari queue (max timeout 100 tick)
    if (xQueueReceive(volumeQueue, &vol, portTICK_PERIOD_MS * 100)) {

      // Kalau mute aktif, treat vol = 0 untuk output
      int displayVol = isMuted ? 0 : vol;

      // === OLED Display ===
      display.clearDisplay();
      display.setTextSize(2);
      display.setTextColor(WHITE);
      display.setCursor(0, 0);

      if (isMuted) {
        display.println("MUTED");
      } else {
        display.print("VOL: ");
        display.print(displayVol);
        display.print("%");
      }

      // Bar indikator volume (pakai displayVol)
      int barLength = map(displayVol, 0, 100, 0, 120);
      display.fillRect(4, 40, barLength, 15, WHITE);
      display.display();

      // === LED BLINK (semakin besar volume, semakin cepat) ===
      int blinkDelay = map(displayVol, 0, 100, 800, 80);
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(blinkDelay / portTICK_PERIOD_MS);
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(blinkDelay / portTICK_PERIOD_MS);

      // === BUZZER >70% (non-mute saja) ===
      if (!isMuted && displayVol > 70) {
        int freq = map(displayVol, 70, 100, 800, 2000);
        tone(BUZZER_PIN, freq, 80);   // beep pendek
      } else {
        noTone(BUZZER_PIN);
      }
    }
  }
}

// ================= SETUP ===================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ESP32-S3 Smart Volume Control (Dual Core + FreeRTOS)");

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Smart Volume Init...");
  display.display();

  // FreeRTOS object
  volumeQueue = xQueueCreate(10, sizeof(int));
  volumeMutex = xSemaphoreCreateMutex();

  if (volumeQueue == NULL || volumeMutex == NULL) {
    Serial.println("Queue/Mutex create failed!");
    while (1);
  }

  // Task ke core
  xTaskCreatePinnedToCore(TaskSensor, "Sensor", 2048, NULL, 1, NULL, 0);  // Core 0
  xTaskCreatePinnedToCore(TaskOutput, "Output", 4096, NULL, 1, NULL, 1);  // Core 1
}

void loop() {
  // kosong — semua dijalankan lewat task
}
