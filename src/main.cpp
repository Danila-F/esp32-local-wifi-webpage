#include <Arduino.h>

#ifndef LED_PIN
#define LED_PIN 2
#endif

#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif

static void setLed(bool on) {
#if LED_ACTIVE_LOW
  digitalWrite(LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  Serial.println();
  Serial.println("ESP32 GPIO2 blink firmware started");
  Serial.print("LED_PIN=");
  Serial.println(LED_PIN);
  Serial.print("LED_ACTIVE_LOW=");
  Serial.println(LED_ACTIVE_LOW);
}

void loop() {
  Serial.println("blink: ON");
  setLed(true);
  delay(500);

  Serial.println("blink: OFF");
  setLed(false);
  delay(500);
}
