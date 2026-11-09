/*
 * SONAR receiver scope - NUCLEO-F446RE, Arduino IDE (STM32duino core)
 * Maximum-rate capture (1.5 MS/s). No external libraries.
 *
 * ADC1 clock = PCLK2/4 = 22.5 MHz (ADC max is 36 MHz)
 * Sampling 3 cycles + 12 conversion = 15 cycles -> 1.5 MS/s
 * TIM2 triggers each conversion, DMA fills the buffer.
 *
 * A0 (PA0) signal in
 * D10 TFT CS   D9 TFT DC   D8 TFT RST   D13 SCK   D11 SDA
 * D5 ENC A     D4 ENC B    D3 ENC SW
 */

#include <SPI.h>
#include <string.h>
#include <stdio.h>

#define TFT_CS  D10
#define TFT_DC  D9
#define TFT_RST D8
#define ENC_A   D5
#define ENC_B   D4
#define ENC_SW  D3
#define SIG_PIN A0

#define WAVE_SPEED 1500.0f   // m/s: 1500 for water, 343 for air

#define W        160
#define H        128
#define BAND     8           // rows sent per SPI burst
#define FS_REQ   1500000UL   // maximum ADC rate with 3-cycle sampling
#define CAP_N    4096        // samples per capture (about 2.7 ms)
#define DISP_N   320         // samples shown (2 per column, about 6 cycles at 30 kHz)
#define HYST_MIN 24          // minimum trigger hysteresis, ADC counts
#define TEXTY    90          // rows 0..89 = wave, rows 90..127 = stats
#define MIDY     45          // centre row of the wave
#define GRID     15

// --- burst handling (sonar is pulsed: most capture windows contain only silence) ---
#define BURST_MIN_SPAN 100   // min Vpp in ADC counts (~80 mV) for a capture to count as a burst
#define MIN_CROSSINGS  3     // need at least 2 full cycles, rejects single noise spikes
#define STAT_MS        300   // text readout refresh interval (stops digits flickering)
#define STALE_MS       3000  // no burst for this long -> clear trace and show "--"
#define FILT_N         5     // readings used for the median filter on frequency and Vpp

// Colours (RGB565)
#define C_BG     0xFFFF
#define C_GRID   0xDEFB
#define C_MID    0x8410
#define C_BORDER 0x4208
#define C_TEXT   0x0000
#define C_RED    0xF800

static uint16_t cap[CAP_N] __attribute__((aligned(4)));
static uint8_t  band[W * BAND * 2];
static int      ymin[W], ymax[W];
static char     lines[4][24];
static volatile int encCount = 0;
static float    gFs = 0;             // actual sample rate, set in adcTimerInit
static float    gFreq = 0;
static uint32_t gMv = 0;
static int      gMid = 2048;
static uint16_t dispBuf[DISP_N];     // last good triggered burst, held on screen
static bool     haveWave = false;
static uint32_t lastGoodMs = 0, lastStatMs = 0;
static int      gainIdx = 1;
static float    fHist[FILT_N], vHist[FILT_N];   // recent raw readings
static uint8_t  hCount = 0, hIdx = 0;

const uint16_t gainTenths[] = {5, 10, 20, 40, 80};             // x0.5 x1 x2 x4 x8
const char*    gainStr[]    = {"x0.5", "x1", "x2", "x4", "x8"};

// Font: each glyph's position in fontChars matches its entry in font[].
const char fontChars[] = "0123456789.:VkHzpxmFTWLusc ";
const uint8_t font[][5] = {
 {0x3E,0x51,0x49,0x45,0x3E}, // 0
 {0x00,0x42,0x7F,0x40,0x00}, // 1
 {0x42,0x61,0x51,0x49,0x46}, // 2
 {0x21,0x41,0x45,0x4B,0x31}, // 3
 {0x18,0x14,0x12,0x7F,0x10}, // 4
 {0x27,0x45,0x45,0x45,0x39}, // 5
 {0x3C,0x4A,0x49,0x49,0x30}, // 6
 {0x01,0x71,0x09,0x05,0x03}, // 7
 {0x36,0x49,0x49,0x49,0x36}, // 8
 {0x06,0x49,0x49,0x29,0x1E}, // 9
 {0x00,0x60,0x60,0x00,0x00}, // .
 {0x00,0x36,0x36,0x00,0x00}, // :
 {0x1F,0x20,0x40,0x20,0x1F}, // V
 {0x7F,0x10,0x28,0x44,0x00}, // k
 {0x7F,0x08,0x08,0x08,0x7F}, // H
 {0x44,0x64,0x54,0x4C,0x44}, // z
 {0x7C,0x14,0x14,0x14,0x08}, // p
 {0x44,0x28,0x10,0x28,0x44}, // x
 {0x7C,0x04,0x18,0x04,0x78}, // m
 {0x7F,0x09,0x09,0x09,0x01}, // F
 {0x01,0x01,0x7F,0x01,0x01}, // T
 {0x3F,0x40,0x38,0x40,0x3F}, // W
 {0x7F,0x40,0x40,0x40,0x40}, // L
 {0x38,0x40,0x40,0x20,0x7C}, // u
 {0x48,0x54,0x54,0x54,0x20}, // s
 {0x38,0x44,0x44,0x44,0x20}, // c
 {0x00,0x00,0x00,0x00,0x00}  // space
};

// ---------- ST7735 low-level ----------
static void tftCmd(uint8_t c, const uint8_t *d, uint8_t n)
{
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  SPI.transfer(c);
  if (n) {
    digitalWrite(TFT_DC, HIGH);
    for (uint8_t i = 0; i < n; i++) SPI.transfer(d[i]);
  }
  digitalWrite(TFT_CS, HIGH);
}

static void tftInit()
{
  digitalWrite(TFT_RST, LOW);  delay(20);
  digitalWrite(TFT_RST, HIGH); delay(120);

  tftCmd(0x01, NULL, 0); delay(150);     // SWRESET
  tftCmd(0x11, NULL, 0); delay(255);     // SLPOUT

  static const uint8_t b1[] = {0x01,0x2C,0x2D}, b3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
  static const uint8_t b4[] = {0x07}, c0[] = {0xA2,0x02,0x84}, c1[] = {0xC5};
  static const uint8_t c2[] = {0x0A,0x00}, c3[] = {0x8A,0x2A}, c4[] = {0x8A,0xEE};
  static const uint8_t c5[] = {0x0E}, madctl[] = {0xA0}, pix16[] = {0x05};
  static const uint8_t e0[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                               0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
  static const uint8_t e1[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                               0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};

  tftCmd(0xB1, b1, 3);
  tftCmd(0xB2, b1, 3);
  tftCmd(0xB3, b3, 6);
  tftCmd(0xB4, b4, 1);
  tftCmd(0xC0, c0, 3);
  tftCmd(0xC1, c1, 1);
  tftCmd(0xC2, c2, 2);
  tftCmd(0xC3, c3, 2);
  tftCmd(0xC4, c4, 2);
  tftCmd(0xC5, c5, 1);
  tftCmd(0x20, NULL, 0);                 // INVOFF (use 0x21 if colours are inverted)
  tftCmd(0x36, madctl, 1);               // landscape
  tftCmd(0x3A, pix16, 1);                // 16-bit colour
  tftCmd(0xE0, e0, 16);
  tftCmd(0xE1, e1, 16);
  tftCmd(0x13, NULL, 0); delay(10);      // NORON
  tftCmd(0x29, NULL, 0); delay(100);     // DISPON
}

// ---------- ADC + timer setup ----------
static void adcTimerInit()
{
  pinMode(SIG_PIN, INPUT_ANALOG);

  RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
  RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
  (void)RCC->APB2ENR;

  // ADC clock = PCLK2 / 4 = 22.5 MHz (must stay at or below 36 MHz)
  ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | (1u << 16);
  ADC1->CR1   = 0;                 // 12-bit, single channel
  ADC1->SMPR2 = 0;                 // channel 0: 3 sampling cycles (fastest)
  ADC1->SQR1  = 0;                 // one conversion
  ADC1->SQR3  = 0;                 // channel 0 first

  // TIM2 update event triggers the ADC at FS_REQ
  uint32_t pclk1  = HAL_RCC_GetPCLK1Freq();
  uint32_t timclk = (RCC->CFGR & RCC_CFGR_PPRE1_2) ? pclk1 * 2 : pclk1;
  uint32_t arr    = timclk / FS_REQ - 1;
  gFs = (float)timclk / (float)(arr + 1);
  TIM2->CR1 = 0;
  TIM2->PSC = 0;
  TIM2->ARR = arr;
  TIM2->CR2 = (2u << 4);           // MMS = update -> TRGO
  TIM2->EGR = TIM_EGR_UG;
}

// ---------- capture one block with DMA ----------
static void captureBlock()
{
  TIM2->CR1 = 0;
  ADC1->CR2 = 0;
  ADC1->SR  = 0;                                 // clear stale overrun/EOC flags
  DMA2_Stream0->CR &= ~1u;
  while (DMA2_Stream0->CR & 1u) {}
  DMA2->LIFCR = 0x3Du;

  DMA2_Stream0->PAR  = (uint32_t)&ADC1->DR;
  DMA2_Stream0->M0AR = (uint32_t)cap;
  DMA2_Stream0->NDTR = CAP_N;
  DMA2_Stream0->FCR  = 0;                        // direct mode
  DMA2_Stream0->CR   = (2u << 16)                // priority high
                     | (1u << 13)                // memory 16-bit
                     | (1u << 11)                // peripheral 16-bit
                     | (1u << 10)                // memory increment
                     | 1u;                       // enable (channel 0, periph->mem)

  ADC1->CR2 = (1u << 0);                         // ADON
  delayMicroseconds(10);                         // ADC stabilisation
  // DMA=1, DDS=1, EXTEN=rising edge, EXTSEL=TIM2_TRGO
  ADC1->CR2 = (1u << 0) | (1u << 8) | (1u << 9) | (1u << 28) | (6u << 24);

  TIM2->CNT = 0;
  TIM2->SR  = 0;
  TIM2->CR1 = TIM_CR1_CEN;

  uint32_t t0 = micros();
  while (!(DMA2->LISR & (1u << 5))) {            // TCIF0
    if (micros() - t0 > 30000) break;            // safety timeout
  }

  TIM2->CR1 = 0;
  ADC1->CR2 = 0;
  DMA2_Stream0->CR &= ~1u;
  DMA2->LIFCR = 0x3Du;
}

// ---------- analysis (burst detect, frequency, Vpp, trigger) ----------
// Median of the n most recent readings: one bad capture cannot move the readout.
static float median(const float *h, int n)
{
  float t[FILT_N];
  for (int i = 0; i < n; i++) t[i] = h[i];
  for (int i = 1; i < n; i++) {                  // insertion sort
    float k = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[n / 2];
}

// Returns true only if the capture holds a real burst with a full display window after
// the trigger point. Otherwise the caller keeps showing the previous good burst.
static bool analyse()
{
  uint32_t sum = 0;
  uint16_t mn = 4095, mx = 0;
  for (int i = 0; i < CAP_N; i++) {
    uint16_t v = cap[i];
    sum += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  int span = mx - mn;
  if (span < BURST_MIN_SPAN) return false;       // silence between bursts

  int mid    = (int)(sum / CAP_N);               // DC level (mean is stable, min/max is not)
  int hyst   = span / 6;
  if (hyst < HYST_MIN) hyst = HYST_MIN;
  int lo     = mid - hyst, hi = mid + hyst;
  int strong = mid - (span * 3) / 10;            // trough that marks a full-amplitude cycle

  float firstT = -1, lastT = -1;
  int   nc = 0, trig = -1;
  bool  armed = false, armedStrong = false;
  for (int i = 1; i < CAP_N; i++) {
    if (cap[i] < lo) {
      armed = true;
      if (cap[i] < strong) armedStrong = true;
    } else if (armed && cap[i] >= hi) {
      float a = cap[i - 1], b = cap[i];
      float frac = (b > a) ? ((float)mid - a) / (b - a) : 0.0f;
      float t = (float)(i - 1) + frac;           // interpolated mid-level crossing
      if (nc == 0) firstT = t;
      lastT = t;
      nc++;
      // trigger on the first cycle that has reached full amplitude (skips the ramp-up),
      // so every burst is drawn with the same shape and phase
      if (trig < 0 && armedStrong && i <= CAP_N - DISP_N) trig = i - 1;
      armed = false;
      armedStrong = false;
    }
  }
  if (trig < 0 || nc < MIN_CROSSINGS) return false;

  // push raw readings into the history and publish the median
  fHist[hIdx] = gFs * (float)(nc - 1) / (lastT - firstT);
  vHist[hIdx] = (float)span * 3300.0f / 4095.0f;
  hIdx = (hIdx + 1) % FILT_N;
  if (hCount < FILT_N) hCount++;
  gFreq = median(fHist, hCount);
  gMv   = (uint32_t)median(vHist, hCount);
  gMid  = mid;
  memcpy(dispBuf, &cap[trig], DISP_N * sizeof(uint16_t));
  return true;
}

// ---------- number formatting ----------
static int putU(char *s, int pos, uint32_t v)
{
  char t[12];
  int n = 0;
  do { t[n++] = '0' + (v % 10); v /= 10; } while (v);
  while (n) s[pos++] = t[--n];
  return pos;
}

static int putFixed(char *s, int pos, float v, int dec)
{
  long m = 1;
  for (int i = 0; i < dec; i++) m *= 10;
  long n = (long)(v * m + 0.5f);
  pos = putU(s, pos, n / m);
  if (dec) {
    s[pos++] = '.';
    long fr = n % m;
    for (int d = dec - 1; d >= 0; d--) {
      long div = 1;
      for (int i = 0; i < d; i++) div *= 10;
      s[pos++] = '0' + (fr / div) % 10;
    }
  }
  return pos;
}

static void buildStats(float freqHz, uint32_t mv)
{
  int p;

  p = 0;
  lines[0][p++] = 'F'; lines[0][p++] = ':'; lines[0][p++] = ' ';
  if (freqHz < 1.0f) {
    lines[0][p++] = '-'; lines[0][p++] = '-';
  } else if (freqHz >= 1000.0f) {
    p = putFixed(lines[0], p, freqHz / 1000.0f, 2);
    lines[0][p++] = 'k'; lines[0][p++] = 'H'; lines[0][p++] = 'z';
  } else {
    p = putFixed(lines[0], p, freqHz, 0);
    lines[0][p++] = 'H'; lines[0][p++] = 'z';
  }
  lines[0][p] = '\0';

  p = 0;
  lines[1][p++] = 'T'; lines[1][p++] = ':'; lines[1][p++] = ' ';
  if (freqHz < 1.0f) {
    lines[1][p++] = '-'; lines[1][p++] = '-';
  } else {
    float us = 1000000.0f / freqHz;
    if (us >= 1000.0f) {
      p = putFixed(lines[1], p, us / 1000.0f, 2);
      lines[1][p++] = 'm'; lines[1][p++] = 's';
    } else {
      p = putFixed(lines[1], p, us, 1);
      lines[1][p++] = 'u'; lines[1][p++] = 's';
    }
  }
  lines[1][p] = '\0';

  p = 0;
  lines[2][p++] = 'W'; lines[2][p++] = 'L'; lines[2][p++] = ':'; lines[2][p++] = ' ';
  if (freqHz < 1.0f) {
    lines[2][p++] = '-'; lines[2][p++] = '-';
  } else {
    float cm = WAVE_SPEED * 100.0f / freqHz;
    if (cm >= 100.0f) {
      p = putFixed(lines[2], p, cm / 100.0f, 2);
      lines[2][p++] = 'm';
    } else {
      p = putFixed(lines[2], p, cm, 1);
      lines[2][p++] = 'c'; lines[2][p++] = 'm';
    }
  }
  lines[2][p] = '\0';

  p = 0;
  lines[3][p++] = 'V'; lines[3][p++] = 'p'; lines[3][p++] = 'p'; lines[3][p++] = ':';
  lines[3][p++] = ' ';
  p = putFixed(lines[3], p, mv / 1000.0f, 2);
  lines[3][p++] = 'V';
  lines[3][p] = '\0';
}

// ---------- waveform columns ----------
static int rowOf(uint16_t v)
{
  int32_t d = (int32_t)v - gMid;
  int y = MIDY - (int)(d * gainTenths[gainIdx] * 42L / (4095L * 10L));
  if (y < 2) y = 2;
  if (y > TEXTY - 2) y = TEXTY - 2;
  return y;
}

// Each screen column shows the min and max of its samples (scope-style envelope).
static void buildColumns()
{
  const int per = DISP_N / W;          // 2 samples per column
  int prevY = rowOf(dispBuf[0]);
  for (int x = 0; x < W; x++) {
    uint16_t vmin = 4095, vmax = 0;
    int base = x * per;
    for (int k = 0; k < per; k++) {
      uint16_t v = dispBuf[base + k];
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    int a = rowOf(vmin), b = rowOf(vmax);
    int top = min(min(a, b), prevY);
    int bot = max(max(a, b), prevY);
    ymin[x] = top;
    ymax[x] = bot;
    prevY = rowOf(dispBuf[base + per - 1]);
  }
}

// ---------- band rendering ----------
static inline void setBandPix(int x, int r, uint16_t c)
{
  int i = (r * W + x) * 2;
  band[i]     = c >> 8;
  band[i + 1] = c & 0xFF;
}

static void drawText(int y0, int tx, int ty, const char *s, uint16_t c)
{
  for (; *s; s++, tx += 6) {
    const char *p = strchr(fontChars, *s);
    if (!p) continue;
    const uint8_t *g = font[p - fontChars];
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 7; row++)
        if (g[col] & (1 << row)) {
          int px = tx + col, py = ty + row;
          if (px >= 0 && px < W && py >= y0 && py < y0 + BAND)
            setBandPix(px, py - y0, c);
        }
  }
}

static void renderBand(int y0)
{
  for (int r = 0; r < BAND; r++) {
    int row = y0 + r;
    for (int x = 0; x < W; x++) {
      uint16_t c = C_BG;
      if (row < TEXTY) {
        if (row == MIDY)                               c = C_MID;
        else if ((row % GRID) == 0 || (x % GRID) == 0) c = C_GRID;
        if (row >= ymin[x] && row <= ymax[x])          c = C_RED;
      }
      if (row == TEXTY)                                c = C_BORDER;
      if (row == 0 || row == H - 1 || x == 0 || x == W - 1) c = C_BORDER;
      setBandPix(x, r, c);
    }
  }
  for (int i = 0; i < 4; i++)
    drawText(y0, 4, TEXTY + 4 + 9 * i, lines[i], C_TEXT);
  drawText(y0, 120, TEXTY + 4, gainStr[gainIdx], C_TEXT);
}

static void tftFlushFrame()
{
  for (int y0 = 0; y0 < H; y0 += BAND) {
    renderBand(y0);
    uint8_t ca[4] = {0, 0, 0, (uint8_t)(W - 1)};
    uint8_t ra[4] = {0, (uint8_t)y0, 0, (uint8_t)(y0 + BAND - 1)};
    tftCmd(0x2A, ca, 4);
    tftCmd(0x2B, ra, 4);

    digitalWrite(TFT_DC, LOW);
    digitalWrite(TFT_CS, LOW);
    SPI.transfer(0x2C);
    digitalWrite(TFT_DC, HIGH);
    SPI.transfer(band, sizeof(band));
    digitalWrite(TFT_CS, HIGH);
  }
}

// ---------- encoder ----------
void encISR()
{
  if (digitalRead(ENC_B)) encCount++;
  else                    encCount--;
}

// ---------- setup ----------
void setup()
{
  pinMode(TFT_CS,  OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC,  OUTPUT);
  pinMode(TFT_RST, OUTPUT);

  pinMode(ENC_A,  INPUT_PULLUP);
  pinMode(ENC_B,  INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, FALLING);

  SPI.begin();
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

  tftInit();
  adcTimerInit();

  for (int x = 0; x < W; x++) { ymin[x] = MIDY; ymax[x] = MIDY; }
  for (int i = 0; i < DISP_N; i++) dispBuf[i] = 2048;
  buildStats(0, 0);
  tftFlushFrame();
}

// ---------- main loop ----------
// Capture continuously (cheap, ~3 ms) and only redraw when a new burst is caught,
// the gain changed, or the signal went stale. The screen never gets wiped by silence.
void loop()
{
  bool redraw = false;

  noInterrupts();
  int e = encCount;
  encCount = 0;
  interrupts();
  if (e) {
    int g = gainIdx + e;
    if (g < 0) g = 0;
    if (g > 4) g = 4;
    if (g != gainIdx) { gainIdx = g; redraw = true; }
  }
  if (digitalRead(ENC_SW) == LOW) {    // press = reset to x1
    if (gainIdx != 1) { gainIdx = 1; redraw = true; }
    delay(150);
  }

  captureBlock();                      // DMA fills cap[] at 1.5 MS/s
  uint32_t now = millis();

  if (analyse()) {
    bool first = !haveWave;
    haveWave   = true;
    lastGoodMs = now;
    redraw     = true;
    if (first || now - lastStatMs >= STAT_MS) {
      buildStats(gFreq, gMv);
      lastStatMs = now;
    }
  } else if (haveWave && now - lastGoodMs > STALE_MS) {
    haveWave = false;                  // transmitter stopped: show flat line and "--"
    for (int i = 0; i < DISP_N; i++) dispBuf[i] = 2048;
    gMid = 2048; gFreq = 0; gMv = 0;
    hCount = 0; hIdx = 0;              // forget old readings
    buildStats(gFreq, gMv);
    redraw = true;
  }

  if (redraw) {
    buildColumns();
    tftFlushFrame();
  }
}