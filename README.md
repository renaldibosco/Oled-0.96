# OLED Lyrics Show

ESP32 + 0.96" SSD1306 OLED that shows animated, timed lyrics in sync with a song playing on your phone.

**Wiring:** VCC to 3V3, GND to GND, SCL to GPIO 22, SDA to GPIO 21

**Install:** open the web flasher at https://renaldibosco.github.io/Oled-0.96/ in Chrome on a laptop, or flash `firmware/oled_lyrics_show.bin` at address `0x0`.

**Use:** connect your phone to WiFi `OLED-Lyrics` (password `12345678`), open `http://192.168.4.1`, pick the song file, paste your lyrics, tap out the timing, save, and press play. The BOOT button also starts and stops the show.
