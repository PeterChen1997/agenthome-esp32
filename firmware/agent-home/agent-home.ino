// AgentHome ESP32 · 桌面 Token / 额度屏  https://github.com/PeterChen1997/agenthome-esp32
// 第 1 页「今日 Token」,第 2 页「模型额度」;点一下屏幕翻页。
// 屏 ILI9341 横屏 320x240,整屏画在 PSRAM 画布上再一次性刷出,避免闪烁。
// 数据由 Mac 端 tools/push_usage.py 经 USB 串口每分钟推送,每行一条:
//   AH1 hm=21:30 today=498984564 prev=648549167 avg=762000000 cc=479100000 cx=19900000 out=1195071 days=540,857,2004,789,0,649,499 dow=0
//     days 是最近 7 天(最后一个是今天)的总量,单位 M;dow 是 7 天里第一天的星期(0=周日)
//   AHM 0|33|Claude|周额度 · 11-21 重置||d97757||3|44
//   AHM 2|29|Gemini|5h 29% · 2:06 后重置|周 24% · 11-21 重置|6a8caf|24|3|39|3|48
//     第 2 页第 i 家(0 Claude、1 Codex、2 Gemini):剩余百分比|名称|第一行说明|第二行说明|颜色(RGB hex)|内圈剩余百分比
//       |紧张程度|时间刻度|内圈紧张程度|内圈时间刻度
//     Claude / Codex 画成横条,只用第一行说明;Gemini 画双层环:外圈 5h、内圈周额度。文字由 Mac 端排好
//     紧张程度:0 不判断(白)、1 够用(绿)、2 偏快(黄)、3 撑不到重置(红),给百分比数字着色
//     时间刻度:离重置还剩的时间占窗口的百分比,画成白色竖线;剩余额度没到这根线就是用得比时间快;-1 不画
//   AHW Codex 剩余最少 · 51%
//     第 2 页底部提示
//   AHS
//     截屏:回 "shot 320 240",接着是整屏 RGB565 原始字节(小端),再回 "end";tools/screenshot.py 存成 PNG
// 板子 LCDwiki ES3C28P(ESP32-S3 2.8" 触摸屏),换板子改下面的引脚:屏 CS=10 DC=46 SCLK=12 MOSI=11 MISO=13 BL=45;触摸 FT6336 SDA=16 SCL=15 RST=18
#include <Wire.h>
#include <U8g2lib.h>  // 必须先于 Arduino_GFX 引入:它据此打开 u8g2 字体与中文支持
#include <Arduino_GFX_Library.h>

#define TFT_CS 10
#define TFT_DC 46
#define TFT_SCK 12
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_BL 45
#define PIN_SDA 16
#define PIN_SCL 15
#define PIN_TP_RST 18
#define FT6336_ADDR 0x38

#define W 320
#define H 240
#define STALE_MS (3 * 60 * 1000UL)  // 3 分钟没收到数据就标「离线」

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

// 配色(GitHub 深色风格)
static const uint16_t C_BG = rgb(0x0d, 0x11, 0x17);
static const uint16_t C_TEXT = rgb(0xe6, 0xed, 0xf3);
static const uint16_t C_MUTED = rgb(0x8b, 0x94, 0x9e);
static const uint16_t C_DIM = rgb(0x6e, 0x76, 0x81);
static const uint16_t C_CHIP = rgb(0x16, 0x1b, 0x22);
static const uint16_t C_TRACK = rgb(0x21, 0x26, 0x2d);
static const uint16_t C_BAR = rgb(0x30, 0x36, 0x3d);
static const uint16_t C_TODAY = rgb(0xd2, 0x99, 0x22);
static const uint16_t C_DOWN = rgb(0x3f, 0xb9, 0x50);
static const uint16_t C_UP = rgb(0xf8, 0x51, 0x49);

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *panel = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1 /* 横屏 */, true /* IPS */);
Arduino_Canvas *gfx = new Arduino_Canvas(W, H, panel);

struct Usage {
  bool valid = false;
  char hm[6] = "--:--";
  double today = 0, prev = 0, avg = 0, cc = 0, cx = 0, out = 0;
  int days[7] = {0};
  int dow = 0;
} usage;

struct Quota {
  bool valid = false;
  int remain = 0;
  int inner = -1;  // 内圈(Gemini 周额度)剩余百分比,-1 表示没有
  int pace = 0, tick = -1, innerPace = 0, innerTick = -1;
  char name[16] = "";
  char line1[48] = "";
  char line2[48] = "";
  uint16_t color = 0;
} quotas[3];

static char warn[96] = "";
static int page = 0;
static unsigned long tapCount = 0;  // 诊断用:开机以来识别到的按下次数
static unsigned long lastRx = 0;
static char line[256];
static size_t lineLen = 0;

// 498984564 → "499.0" + "M";保留 1 位小数,够桌面余光看
static void fmtTokens(double n, char *num, size_t nlen, char *unit) {
  const char *u = "";
  double v = n;
  if (n >= 1e9) { v = n / 1e9; u = "B"; }
  else if (n >= 1e6) { v = n / 1e6; u = "M"; }
  else if (n >= 1e3) { v = n / 1e3; u = "K"; }
  if (*u) snprintf(num, nlen, "%.1f", v); else snprintf(num, nlen, "%.0f", v);
  strcpy(unit, u);
}

static void fmtShort(double n, char *buf, size_t len) {
  char num[16], unit[2];
  fmtTokens(n, num, sizeof(num), unit);
  snprintf(buf, len, "%s%s", num, unit);
}

static int textWidth(const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 100, &x1, &y1, &w, &h);
  return w;
}

static void printCentered(const char *s, int cx, int y) {
  gfx->setCursor(cx - textWidth(s) / 2, y);
  gfx->print(s);
}

static void drawHeader(const char *title) {
  bool stale = !usage.valid || millis() - lastRx > STALE_MS;
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(12, 18);
  gfx->print(title);
  char status[24];
  snprintf(status, sizeof(status), "%s · %s", usage.hm, !usage.valid ? "等待数据" : stale ? "离线" : "已同步");
  gfx->setTextColor(stale ? C_UP : C_MUTED);
  gfx->setCursor(W - 12 - textWidth(status), 18);
  gfx->print(status);
}

// 底部两个小圆点,亮的是当前页
static void drawDots() {
  for (int i = 0; i < 2; i++) gfx->fillCircle(W / 2 - 5 + i * 10, H - 5, 2, i == page ? C_TEXT : C_BAR);
}

static void drawChip(int x, int y, int w, const char *label, double value) {
  gfx->fillRoundRect(x, y, w, 38, 6, C_CHIP);
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(x + 7, y + 13);
  gfx->print(label);
  char buf[16];
  fmtShort(value, buf, sizeof(buf));
  gfx->setFont(u8g2_font_helvB14_tr);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(x + 7, y + 33);
  gfx->print(buf);
}

static void renderTokens() {
  drawHeader("AGENT HOME · 今日 Token");

  // 大数字
  char num[16], unit[2];
  fmtTokens(usage.today, num, sizeof(num), unit);
  gfx->setFont(u8g2_font_logisoso46_tn);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(12, 76);
  gfx->print(usage.valid ? num : "--");
  int nx = 12 + textWidth(usage.valid ? num : "--") + 6;
  gfx->setFont(u8g2_font_logisoso16_tr);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(nx, 76);
  gfx->print(unit);

  // 环比 + 7 日均
  if (usage.valid && usage.prev > 0) {
    double pct = (usage.today - usage.prev) / usage.prev * 100.0;
    bool down = pct < 0;
    uint16_t c = down ? C_DOWN : C_UP;
    int ty = 87;
    if (down) gfx->fillTriangle(12, ty, 22, ty, 17, ty + 8, c);
    else gfx->fillTriangle(12, ty + 8, 22, ty + 8, 17, ty, c);
    char avg[16], buf[64];
    fmtShort(usage.avg, avg, sizeof(avg));
    snprintf(buf, sizeof(buf), "%.0f%% 比昨天 · 7 日均 %s", fabs(pct), avg);
    gfx->setFont(u8g2_font_cubic11_h_cjk);
    gfx->setTextColor(c);
    gfx->setCursor(27, ty + 9);
    gfx->print(buf);
  }

  // 三个分项
  const int cw = (W - 24 - 12) / 3;
  drawChip(12, 104, cw, "Claude Code", usage.cc);
  drawChip(12 + cw + 6, 104, cw, "Codex", usage.cx);
  drawChip(12 + (cw + 6) * 2, 104, cw, "输出", usage.out);

  // 7 天柱状图
  static const char *WEEK[] = {"日", "一", "二", "三", "四", "五", "六"};
  int maxv = 1;
  for (int i = 0; i < 7; i++) if (usage.days[i] > maxv) maxv = usage.days[i];
  const int top = 150, bottom = 210, gap = 6;
  const int bw = (W - 24 - gap * 6) / 7;
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  for (int i = 0; i < 7; i++) {
    int x = 12 + i * (bw + gap);
    int h = (int)((long)usage.days[i] * (bottom - top) / maxv);
    if (h < 2) h = 2;
    gfx->fillRoundRect(x, bottom - h, bw, h, 3, i == 6 ? C_TODAY : C_BAR);
    const char *lbl = i == 6 ? "今" : WEEK[(usage.dow + i) % 7];
    gfx->setTextColor(C_DIM);
    printCentered(lbl, x + bw / 2, 226);
  }
}

static uint16_t paceColor(int pace) {
  static const uint16_t COLORS[] = {C_TEXT, C_DOWN, C_TODAY, C_UP};
  return COLORS[constrain(pace, 0, 3)];
}

// 环上的时间刻度:按时间平均分配的话,剩余额度应该到的位置
static void drawRingTick(int cx, int cy, int r, int thick, int tick) {
  if (tick < 0) return;
  float a = (270 + (100 - tick) * 3.6f) * DEG_TO_RAD;
  for (int d = 0; d < 2; d++) {
    float rr0 = r - thick - 2 + d * 0.5f, rr1 = r + 2;
    float ox = d ? -sinf(a) : 0, oy = d ? cosf(a) : 0;  // 第二笔沿切向挪 1 像素,画成 2 像素宽
    gfx->drawLine(cx + rr0 * cosf(a) + ox, cy + rr0 * sinf(a) + oy, cx + rr1 * cosf(a) + ox, cy + rr1 * sinf(a) + oy, C_TEXT);
  }
}

// 底部图例:三种紧张程度的颜色含义
static void drawPaceLegend(int y) {
  static const char *LABELS[] = {"够用", "偏快", "撑不到重置"};
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  int total = 0;
  for (int k = 0; k < 3; k++) total += 9 + textWidth(LABELS[k]) + (k < 2 ? 12 : 0);
  int x = W / 2 - total / 2;
  for (int k = 0; k < 3; k++) {
    gfx->fillCircle(x + 3, y - 4, 3, paceColor(k + 1));
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(x + 9, y);
    gfx->print(LABELS[k]);
    x += 9 + textWidth(LABELS[k]) + 12;
  }
}

// 剩余部分着色、已用部分暗色;已用从 12 点方向起顺时针扣
static void drawRing(int cx, int cy, int r, int thick, int remain, uint16_t color) {
  gfx->fillArc(cx, cy, r, r - thick, 0, 360, C_TRACK);
  if (remain > 0) gfx->fillArc(cx, cy, r, r - thick, 270 + (100 - remain) * 3.6f, 630, color);
}

// 颜色和白色各半,给 Gemini 内圈用
static uint16_t lighter(uint16_t c) {
  return (((c >> 11) + 31) / 2) << 11 | (((c >> 5 & 0x3F) + 63) / 2) << 5 | ((c & 0x1F) + 31) / 2;
}

// 左侧卡片:名称、剩余百分比、横条、说明
static void drawCard(int y, int i) {
  const Quota &q = quotas[i];
  gfx->fillRoundRect(12, y, 148, 70, 6, C_CHIP);
  gfx->setFont(u8g2_font_helvB12_tr);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(22, y + 18);
  gfx->print(q.valid ? q.name : "--");
  char pct[8];
  snprintf(pct, sizeof(pct), q.valid ? "%d%%" : "--", q.remain);
  gfx->setFont(u8g2_font_logisoso16_tr);
  gfx->setTextColor(q.valid ? paceColor(q.pace) : C_TEXT);
  gfx->setCursor(150 - textWidth(pct), y + 20);
  gfx->print(pct);
  gfx->fillRoundRect(22, y + 30, 128, 8, 4, C_TRACK);
  if (q.valid && q.remain > 0) gfx->fillRoundRect(22, y + 30, max(8, 128 * q.remain / 100), 8, 4, q.color);
  if (q.valid && q.tick >= 0) gfx->fillRect(22 + 128 * q.tick / 100 - 1, y + 27, 2, 15, C_TEXT);
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(22, y + 56);
  gfx->print(q.line1);
}

// 左边 Claude / Codex 两张周额度卡片,右边 Gemini 双层环(外圈 5h、内圈周额度)
static void renderQuotas() {
  drawHeader("AGENT HOME · 模型额度");
  drawCard(40, 0);
  drawCard(120, 1);

  const Quota &g = quotas[2];
  const int cx = 242, cy = 92;
  uint16_t innerColor = lighter(g.color);
  drawRing(cx, cy, 54, 10, g.valid ? g.remain : 0, g.color);
  drawRing(cx, cy, 40, 8, g.valid ? g.inner : 0, innerColor);
  if (g.valid) {
    drawRingTick(cx, cy, 54, 10, g.tick);
    drawRingTick(cx, cy, 40, 8, g.innerTick);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), g.valid ? "%d%%" : "--", g.remain);
  gfx->setFont(u8g2_font_logisoso20_tr);
  gfx->setTextColor(g.valid ? paceColor(g.pace) : C_TEXT);
  printCentered(pct, cx, cy + 10);
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextColor(C_MUTED);
  printCentered("5h 剩余", cx, cy + 25);
  gfx->setFont(u8g2_font_helvB12_tr);
  gfx->setTextColor(C_TEXT);
  printCentered(g.valid ? g.name : "Gemini", cx, 166);
  // 两行说明前各一个色点,对应外圈 / 内圈
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  const char *rows[2] = {g.line1, g.line2};
  uint16_t dots[2] = {g.color, innerColor};
  int paces[2] = {g.pace, g.innerPace};
  for (int k = 0; k < 2; k++) {
    if (!rows[k][0]) continue;
    int y = 184 + 16 * k;
    int x = cx - (textWidth(rows[k]) + 9) / 2;
    gfx->fillCircle(x + 3, y - 4, 3, dots[k]);
    gfx->setTextColor(paceColor(paces[k]));
    gfx->setCursor(x + 9, y);
    gfx->print(rows[k]);
  }
  // 数据过期时显示提示,平时显示紧张程度图例
  if (warn[0]) {
    gfx->setTextColor(C_TODAY);
    printCentered(warn, W / 2, 226);
  } else {
    drawPaceLegend(226);
  }
}

static void render() {
  gfx->fillScreen(C_BG);
  gfx->setUTF8Print(true);
  if (page == 0) renderTokens(); else renderQuotas();
  drawDots();
  gfx->flush();
}

static double field(const char *key) {
  char pat[16];
  snprintf(pat, sizeof(pat), " %s=", key);
  const char *p = strstr(line, pat);
  return p ? atof(p + strlen(pat)) : 0;
}

static bool parseTokens() {
  const char *hm = strstr(line, " hm=");
  if (hm) { strncpy(usage.hm, hm + 4, 5); usage.hm[5] = 0; }
  usage.today = field("today");
  usage.prev = field("prev");
  usage.avg = field("avg");
  usage.cc = field("cc");
  usage.cx = field("cx");
  usage.out = field("out");
  usage.dow = (int)field("dow") % 7;
  const char *d = strstr(line, " days=");
  if (d) {
    d += 6;
    for (int i = 0; i < 7 && d; i++) {
      usage.days[i] = atoi(d);
      d = strchr(d, ',');
      if (d) d++;
    }
  }
  usage.valid = true;
  return true;
}

// 把 src 里到下一个 '|' 为止的内容拷进 dst,返回下一段的起点;没有更多段时返回 NULL
static const char *nextPart(const char *src, char *dst, size_t len) {
  if (!src) { dst[0] = 0; return NULL; }
  const char *bar = strchr(src, '|');
  size_t n = bar ? (size_t)(bar - src) : strlen(src);
  if (n >= len) n = len - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
  return bar ? bar + 1 : NULL;
}

static bool parseQuota() {
  char part[48];
  const char *p = nextPart(line + 4, part, sizeof(part));
  int i = atoi(part);
  if (i < 0 || i > 2 || !p) return false;
  Quota &q = quotas[i];
  p = nextPart(p, part, sizeof(part));
  q.remain = constrain(atoi(part), 0, 100);
  p = nextPart(p, q.name, sizeof(q.name));
  p = nextPart(p, q.line1, sizeof(q.line1));
  p = nextPart(p, q.line2, sizeof(q.line2));
  p = nextPart(p, part, sizeof(part));
  long hex = strtol(part, NULL, 16);
  q.color = rgb((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
  p = nextPart(p, part, sizeof(part));
  q.inner = part[0] ? constrain(atoi(part), 0, 100) : -1;
  int *rest[] = {&q.pace, &q.tick, &q.innerPace, &q.innerTick};
  for (int k = 0; k < 4; k++) {
    p = nextPart(p, part, sizeof(part));
    *rest[k] = part[0] ? constrain(atoi(part), -1, 100) : (k % 2 ? -1 : 0);
  }
  q.valid = true;
  return true;
}

static uint8_t touchCount();

// Arduino_GFX 的 ILI9341 默认初始化是给 TN 屏的:不写 gamma 表,电源/VCOM 也不同,
// 在本板 IPS 屏上暗部被大幅提亮,深色背景显示成浅灰蓝。这里按厂家给的 IPS 初始化
// (lcdwiki.com/res/ES3C28P/ILI9341V_Init.txt)补写电源、VCOM、帧率和 gamma。
static void applyVendorPanelTuning() {
  static const uint8_t regs[] = {
      // 寄存器, 字节数, 数据...
      0xC0, 1, 0x13,                                // Power control 1
      0xC1, 1, 0x13,                                // Power control 2
      0xC5, 2, 0x22, 0x35,                          // VCOM control 1
      0xC7, 1, 0xBD,                                // VCOM control 2
      0xB1, 2, 0x00, 0x1B,                          // Frame rate
      0xF2, 1, 0x00,                                // 3Gamma off
      0x26, 1, 0x01,                                // Gamma curve 1
      0xE0, 15, 0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00,
      0xE1, 15, 0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F,
  };
  bus->beginWrite();
  for (size_t i = 0; i < sizeof(regs);) {
    uint8_t cmd = regs[i++], n = regs[i++];
    bus->writeCommand(cmd);
    for (uint8_t k = 0; k < n; k++) bus->write(regs[i++]);
  }
  bus->endWrite();
}

// 诊断:读一次触摸芯片,回报 I2C 结果和原始寄存器,便于远程排查
static void printDiag() {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x02);
  uint8_t err = Wire.endTransmission(false);
  uint8_t got = Wire.requestFrom((int)FT6336_ADDR, 5);
  Serial.printf("diag i2c_err=%u got=%u regs=", err, got);
  while (Wire.available()) Serial.printf("%02x ", Wire.read());
  Serial.printf("page=%d taps=%lu lastRx=%lus ago\n", page, tapCount, (millis() - lastRx) / 1000);
}

// 解析一行;格式不对就整行丢弃,屏上保持旧数据
static bool parseLine() {
  if (strcmp(line, "AH?") == 0) { printDiag(); return true; }
  if (strcmp(line, "AHP") == 0) {  // 远程翻页,等同点一下屏幕
    page ^= 1;
    unsigned long t0 = millis();
    render();
    Serial.printf("page %d rendered in %lums\n", page, millis() - t0);
    return true;
  }
  if (strcmp(line, "AHS") == 0) {
    Serial.printf("shot %d %d\n", W, H);
    Serial.setTxTimeoutMs(1000);  // 平时是 0(不阻塞),截屏这 150KB 得等主机读完
    Serial.write((const uint8_t *)gfx->getFramebuffer(), W * H * 2);
    Serial.setTxTimeoutMs(0);
    Serial.println("\nend");
    return true;
  }
  if (strncmp(line, "AH1 ", 4) == 0) return parseTokens();
  if (strncmp(line, "AHM ", 4) == 0) return parseQuota();
  if (strncmp(line, "AHW ", 4) == 0) {
    strncpy(warn, line + 4, sizeof(warn) - 1);
    warn[sizeof(warn) - 1] = 0;
    return true;
  }
  return false;
}

// FT6336 当前触点数(寄存器 0x02 低 4 位)
static uint8_t touchCount() {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((int)FT6336_ADDR, 1);
  return Wire.available() ? (Wire.read() & 0x0F) : 0;
}

void setup() {
  Serial.setRxBufferSize(2048);  // 默认 256 字节,重画屏幕时一次推送的几行会溢出
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // 没人读串口时不阻塞
  if (!gfx->begin()) Serial.println("canvas alloc failed");
  applyVendorPanelTuning();
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(120);
  Wire.begin(PIN_SDA, PIN_SCL, 400000);

  render();
  Serial.println("agent-home ready");
}

void loop() {
  static unsigned long lastRender = 0;
  static uint8_t lastTouch = 0;
  bool dirty = false;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        line[lineLen] = 0;
        if (parseLine()) {
          lastRx = millis();
          dirty = true;
          Serial.println("ok");
        } else {
          Serial.println("bad line");
        }
        lineLen = 0;
      }
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }

  // 手指按下的那一刻翻页(按住不放不会连翻)
  uint8_t t = touchCount();
  if (t && !lastTouch) {
    tapCount++;
    page ^= 1;
    dirty = true;
    Serial.printf("page %d\n", page);
  }
  lastTouch = t;

  // 每 10 秒重画一次,让「离线」状态能及时出现
  if (dirty || millis() - lastRender > 10000) {
    render();
    lastRender = millis();
  }
  delay(20);
}
