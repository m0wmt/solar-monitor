# solar-monitor
ESP32 with Lilygo TTGO display to display some simple solar information.

Due to problems getting graphics libraries to work with the ESP-IDF (LVGL for one) this is currently written using the Arduino IDE (board selected is TTGO LoRa32-OLED v1) and TFT_eSPI graphics library.

Currently have 3 screens, one for PV being generated now, one for PV total today and a simple text screen of IP address ESP32 has, PV now and PV total.

<p align="left">
  <img src="https://github.com/m0wmt/solar-monitor/blob/main/now.jpeg" width="350" title="PV Now">
  <img src="https://github.com/m0wmt/solar-monitor/blob/main/today.jpeg" width="350" title="PV Total Today">
  <img src="https://github.com/m0wmt/solar-monitor/blob/main/info.jpeg" width="350" title="Text Information Screen">
</p>


