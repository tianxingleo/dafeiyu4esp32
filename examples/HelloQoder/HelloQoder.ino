#include <M5Unified.h>

void setup() {
  auto config = M5.config();
  M5.begin(config);

  auto& display = M5.Display;
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(3);
  display.setTextDatum(middle_center);
  display.drawString("Hello,Qoder", display.width() / 2, display.height() / 2);
}

void loop() {
  delay(1000);
}
