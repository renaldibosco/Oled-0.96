// OLED Lyrics Show - ESP32 + 0.96" SSD1306 OLED (I2C)
// Wiring:  VCC -> 3V3 | GND -> GND | SCL -> GPIO 22 | SDA -> GPIO 21
//
// The ESP32 makes its own WiFi:  name "OLED-Lyrics", password "12345678".
// Connect your phone to it and open  http://192.168.4.1
// On that page you pick the song file, paste the lyrics, tap in time to the
// song to set the timing, and save. After that, pressing play on the page
// plays the song on the phone and the animated lyrics on the OLED together.
// The BOOT button also starts / stops the show from the beginning.

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define AP_NAME    "OLED-Lyrics"
#define AP_PASS    "12345678"
#define OLED_ADDR  0x3C      // try 0x3D if the screen stays black
#define START_BTN  0         // BOOT button
#define MAX_LINES  150
#define LYRIC_H    48        // top 48 px for lyrics, the rest for the equalizer
#define END_HOLD   6000      // ms the last line stays before the show ends

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WebServer server(80);

struct Lyric { uint32_t ms; String text; };
Lyric lyr[MAX_LINES];
int lyrCount = 0;

bool playing = false;
uint32_t startMs = 0;      // millis() value that matches song time 0
int curIdx = -1;
uint32_t animStart = 0;
int effect = 0;

String rows[8];
int rowCount = 0;
int textSize = 1;

float bars[16];
float barTarget[16];
uint32_t lastBarMs = 0;

// ---------------------------------------------------------------- web page
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OLED Lyrics</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:14px;max-width:560px;margin:auto}
h2{margin:6px 0 2px}h3{margin:18px 0 6px;color:#6cf}
textarea{width:100%;box-sizing:border-box;height:150px;background:#000;color:#eee;border:1px solid #555;border-radius:6px;padding:8px;font-size:14px}
button{background:#06c;color:#fff;border:0;border-radius:8px;padding:12px 14px;font-size:16px;margin:6px 6px 6px 0}
#tap{width:100%;padding:26px;font-size:22px;background:#c40}
audio{width:100%;margin-top:8px}
#cur{min-height:44px;font-size:18px;color:#fc6;margin:6px 0}
#msg{color:#6f6;min-height:20px}small{color:#aaa}
</style></head><body>
<h2>OLED Lyrics</h2><div id="msg"></div>
<h3>1. Pick the song from your phone</h3>
<input type="file" id="f" accept="audio/*">
<audio id="a" controls></audio>
<h3>2. Paste the lyrics (one line per row)</h3>
<small>English letters only. Keep lines short for big text.</small>
<textarea id="raw"></textarea>
<h3>3. Set the timing</h3>
<small>Press Start, then hit TAP at the moment each line begins.</small><br>
<button id="sync">Start timing</button>
<div id="cur"></div>
<button id="tap">TAP</button>
<h3>4. Timed lyrics</h3>
<small>You can edit times by hand: [min:sec] text</small>
<textarea id="lrc"></textarea>
<button id="save">Save to ESP32</button>
<h3>5. Play</h3>
<small>Press play on the song above and the OLED follows it.</small><br>
<button id="go">Start OLED only</button><button id="stop">Stop</button>
<script>
const $=id=>document.getElementById(id),a=$('a');
let syncing=false,lines=[],i=0,out=[],t0=0,hasAudio=false;
const msg=t=>$('msg').textContent=t;
$('f').onchange=e=>{const f=e.target.files[0];if(f){a.src=URL.createObjectURL(f);hasAudio=true;}};
const now=()=>hasAudio?a.currentTime:(performance.now()-t0)/1000;
const fmt=s=>{const m=Math.floor(s/60);return '['+m+':'+(s-m*60).toFixed(2).padStart(5,'0')+']';};
const show=()=>$('cur').textContent=syncing?'Next: '+lines[i]:'';
$('sync').onclick=()=>{lines=$('raw').value.split('\n').map(x=>x.trim()).filter(x=>x);
 if(!lines.length){msg('Paste the lyrics first');return;}
 syncing=true;i=0;out=[];t0=performance.now();if(hasAudio){a.currentTime=0;a.play();}show();msg('Timing...');};
$('tap').onclick=()=>{if(!syncing)return;out.push(fmt(now())+' '+lines[i]);i++;
 if(i>=lines.length){syncing=false;$('lrc').value=out.join('\n');if(hasAudio)a.pause();msg('Timing done. Now press Save.');}show();};
$('save').onclick=()=>fetch('/save',{method:'POST',headers:{'Content-Type':'text/plain'},body:$('lrc').value}).then(r=>r.text()).then(msg);
$('go').onclick=()=>fetch('/play?t=0');
$('stop').onclick=()=>fetch('/stop');
const send=()=>{if(!syncing&&hasAudio&&!a.paused)fetch('/play?t='+Math.round(a.currentTime*1000));};
a.onplay=send;a.onseeked=send;a.onpause=()=>{if(!syncing)fetch('/stop');};setInterval(send,4000);
fetch('/lyrics').then(r=>r.text()).then(t=>$('lrc').value=t);
</script></body></html>)HTML";

// ---------------------------------------------------------------- lyrics
// Each line looks like:  [1:23.45] WORDS HERE
void parseLyrics(const String& s) {
  lyrCount = 0;
  int pos = 0, L = s.length();
  while (pos < L && lyrCount < MAX_LINES) {
    int nl = s.indexOf('\n', pos);
    if (nl < 0) nl = L;
    String line = s.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    int close = line.indexOf(']');
    int colon = line.indexOf(':');
    if (!line.startsWith("[") || close < 0 || colon < 0 || colon > close) continue;
    long mins = line.substring(1, colon).toInt();
    float secs = line.substring(colon + 1, close).toFloat();
    String raw = line.substring(close + 1);
    String text = "";
    for (unsigned int k = 0; k < raw.length(); k++) {
      char c = raw[k];
      if (c >= 32 && c < 127) text += c;   // the OLED font only has English characters
    }
    text.trim();
    lyr[lyrCount].ms = (uint32_t)(mins * 60000L + (long)(secs * 1000.0f));
    lyr[lyrCount].text = text;
    lyrCount++;
  }
}

void loadLyrics() {
  File f = LittleFS.open("/lyrics.txt", "r");
  if (f) {
    parseLyrics(f.readString());
    f.close();
  }
  if (lyrCount == 0) {
    parseLyrics("[0:00.5] HELLO\n[0:02.5] CONNECT TO WIFI\n[0:05] OLED-Lyrics\n"
                "[0:07.5] THEN OPEN 192.168.4.1\n[0:10.5] AND ADD YOUR SONG");
  }
}

// ---------------------------------------------------------------- layout
int wrapText(const String& t, int maxChars, int maxRows, bool& tooLong) {
  int n = 0, i = 0, L = t.length();
  String row = "";
  tooLong = false;
  while (i < L) {
    while (i < L && t[i] == ' ') i++;
    int j = i;
    while (j < L && t[j] != ' ') j++;
    if (j == i) break;
    String w = t.substring(i, j);
    i = j;
    if ((int)w.length() > maxChars) tooLong = true;
    if (row.length() == 0) {
      row = w;
    } else if ((int)(row.length() + 1 + w.length()) <= maxChars) {
      row += " " + w;
    } else {
      if (n < maxRows) rows[n++] = row; else tooLong = true;
      row = w;
    }
  }
  if (row.length() > 0) {
    if (n < maxRows) rows[n++] = row; else tooLong = true;
  }
  return n;
}

// Uses the biggest text size that still fits the line on screen.
void layoutLine(const String& t) {
  for (int s = 3; s >= 1; s--) {
    bool tooLong;
    int n = wrapText(t, 128 / (6 * s), LYRIC_H / (8 * s), tooLong);
    if (!tooLong || s == 1) {
      rowCount = n;
      textSize = s;
      return;
    }
  }
}

// ---------------------------------------------------------------- drawing
void drawLyric(uint32_t el) {
  int xOff = 0, yOff = 0, count = 10000;
  bool inverted = false;
  switch (effect) {
    case 0: count = el / 28 + 1; break;                                               // typewriter
    case 1: if (el < 260) { float p = 1 - el / 260.0f; xOff = (int)(128 * p * p); } break;  // slide in
    case 2: if (el < 260) { float p = 1 - el / 260.0f; yOff = (int)(50 * p * p); } break;   // rise up
    case 3: inverted = el < 140; break;                                               // flash
  }
  if (inverted) display.fillRect(0, 0, 128, LYRIC_H, SSD1306_WHITE);
  display.setTextSize(textSize);
  display.setTextColor(inverted ? SSD1306_BLACK : SSD1306_WHITE);
  display.setTextWrap(false);
  int y0 = (LYRIC_H - rowCount * 8 * textSize) / 2;
  if (y0 < 0) y0 = 0;
  int left = count;
  for (int r = 0; r < rowCount && left > 0; r++) {
    int len = rows[r].length();
    int x = (128 - len * 6 * textSize) / 2;
    if (x < 0) x = 0;
    display.setCursor(x + xOff, y0 + r * 8 * textSize + yOff);
    display.print(rows[r].substring(0, min(len, left)));
    left -= len + 1;
  }
}

void drawBars(bool active) {
  uint32_t now = millis();
  if (now - lastBarMs > 90) {
    lastBarMs = now;
    for (int i = 0; i < 16; i++) {
      barTarget[i] = active ? random(1, 12) : 2.5f + 2.0f * sin(now / 400.0f + i * 0.6f);
    }
  }
  display.fillRect(0, LYRIC_H + 1, 128, 64 - LYRIC_H - 1, SSD1306_BLACK);
  for (int i = 0; i < 16; i++) {
    bars[i] += (barTarget[i] - bars[i]) * 0.35f;
    int h = (int)bars[i];
    if (h < 1) h = 1;
    display.fillRect(i * 8 + 1, 61 - h, 6, h, SSD1306_WHITE);
  }
}

void drawIdle() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);   display.print("WiFi: " AP_NAME);
  display.setCursor(0, 11);  display.print("Pass: " AP_PASS);
  display.setCursor(0, 22);  display.print("Open: 192.168.4.1");
  display.setCursor(0, 36);  display.print("BOOT = play  (");
  display.print(lyrCount);
  display.print(")");
  drawBars(false);
}

// ---------------------------------------------------------------- control
void startShow(uint32_t offsetMs) {
  startMs = millis() - offsetMs;
  if (!playing) curIdx = -2;   // forces the first line to animate in
  playing = true;
}

void stopShow() { playing = false; }

void setup() {
  pinMode(START_BTN, INPUT_PULLUP);
  Wire.begin(21, 22);
  Wire.setClock(400000);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.display();

  LittleFS.begin(true);
  loadLyrics();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, AP_PASS);

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/lyrics", []() {
    File f = LittleFS.open("/lyrics.txt", "r");
    String s = f ? f.readString() : String("");
    if (f) f.close();
    server.send(200, "text/plain", s);
  });
  server.on("/save", HTTP_POST, []() {
    String body = server.arg("plain");
    File f = LittleFS.open("/lyrics.txt", "w");
    if (f) { f.print(body); f.close(); }
    stopShow();
    loadLyrics();
    server.send(200, "text/plain", "Saved " + String(lyrCount) + " lines to the ESP32");
  });
  server.on("/play", []() {
    startShow((uint32_t)server.arg("t").toInt());
    server.send(200, "text/plain", "ok");
  });
  server.on("/stop", []() {
    stopShow();
    server.send(200, "text/plain", "ok");
  });
  server.begin();
}

void loop() {
  server.handleClient();

  // BOOT button: start from the beginning, or stop
  static bool lastBtn = HIGH;
  static uint32_t lastPress = 0;
  bool btn = digitalRead(START_BTN);
  if (btn == LOW && lastBtn == HIGH && millis() - lastPress > 300) {
    lastPress = millis();
    if (playing) stopShow(); else startShow(0);
  }
  lastBtn = btn;

  static uint32_t lastFrame = 0;
  if (millis() - lastFrame < 33) return;   // about 30 frames per second
  lastFrame = millis();

  display.clearDisplay();

  if (playing && lyrCount > 0) {
    uint32_t t = millis() - startMs;
    uint32_t total = lyr[lyrCount - 1].ms + END_HOLD;
    if (t > total) {
      stopShow();
    } else {
      int idx = -1;
      for (int i = 0; i < lyrCount; i++) {
        if (lyr[i].ms <= t) idx = i; else break;
      }
      if (idx != curIdx) {
        curIdx = idx;
        animStart = millis();
        if (idx >= 0) {
          effect = idx % 4;
          layoutLine(lyr[idx].text);
        }
      }
      if (curIdx >= 0) drawLyric(millis() - animStart);
      drawBars(true);
      display.drawFastHLine(0, 63, (int)((uint64_t)t * 128 / total), SSD1306_WHITE);
    }
  }

  if (!playing) drawIdle();
  display.display();
}
