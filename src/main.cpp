// Waze HUD cho ESP32 CYD 2.8" (ESP32-2432S028)
// Nhận HLP/1 (JSON mỗi dòng) từ Waze Mod qua Bluetooth Classic SPP rồi vẽ lên màn hình.
// Chữ có dấu được đổi sang không dấu vì font dựng sẵn của TFT_eSPI chỉ có ASCII.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>

#include "util.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled for this build
#endif
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Bluetooth (SPP) is not available for this build
#endif

#define DEVICE_NAME "WazeHUD"
#define FW_VERSION "1.0.0"
#ifndef HUD_ROTATION
#define HUD_ROTATION 1  // 1 = ngang; đổi thành 3 nếu muốn xoay ngược 180 độ
#endif

// ---------------------------------------------------------------- màu sắc
static const uint16_t COL_BG = TFT_BLACK;
static const uint16_t COL_FG = TFT_WHITE;
static const uint16_t COL_DIM = 0x7BEF;  // xám
static const uint16_t COL_OK = TFT_GREEN;
static const uint16_t COL_WARN = TFT_YELLOW;
static const uint16_t COL_BAD = TFT_RED;
static const uint16_t COL_ARROW = TFT_CYAN;
static const uint16_t COL_AMBER = 0xFD20;  // cam

// ---------------------------------------------------------------- bố cục (màn 320x240)
static const int SCR_W = 320;
static const int SCR_H = 240;
static const int TOP_H = 28;
static const int SPD_Y = 32;
static const int SPD_AREA_W = 208;
static const int SIGN_CX = 264;
static const int SIGN_CY = 82;
static const int SIGN_R = 48;
static const int LOW_Y = 134;
static const int ST_Y = 212;

// ---------------------------------------------------------------- trạng thái
struct HudState {
  bool nav;
  int spd, lim;
  bool over;
  int trn, trn2, dst, ex;
  char st[48], st2[48], eta[10];
  int rmin, rm;
  float rkm;
  bool avg;
  int avgL;
  int alr, alrD, alrV;
};

static HudState hud;
static TFT_eSPI tft;
static BluetoothSerial SerialBT;

static bool wasUp = false;
static bool sendDevPending = false;
static uint32_t lastRx = 0;
static long lastSess = -1;

static char rxBuf[512];
static size_t rxLen = 0;
static bool rxOverflow = false;

enum LinkState { LINK_WAIT, LINK_LIVE, LINK_STALE };

static void resetState() {
  memset(&hud, 0, sizeof(hud));
  hud.dst = -1;
  hud.rmin = -1;
  hud.rm = -1;
  hud.rkm = -1;
  hud.alrD = -1;
}

// ---------------------------------------------------------------- gửi dữ liệu
static void sendLine(const char* line) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "%s\n", line);
  if (n <= 0 || n >= (int)sizeof(buf)) return;
  SerialBT.write((const uint8_t*)buf, (size_t)n);
}

static void sendDev() {
  char line[480];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"t\":\"dev\",\"name\":\"%s\",\"fw\":\"%s\",\"proto\":[1],"
           "\"disp\":{\"w\":%d,\"h\":%d,\"color\":1},"
           "\"want\":{\"rate\":4,\"fields\":[\"nav\",\"spd\",\"lim\",\"over\",\"trn\",\"trn2\","
           "\"dst\",\"exit\",\"st\",\"st2\",\"eta\",\"rmin\",\"rm\",\"rkm\",\"avg\",\"avgL\","
           "\"alr\",\"alrD\",\"alrV\"]},"
           "\"can\":[\"speed\",\"limit\",\"turn\",\"street\",\"eta\",\"avgzone\",\"alerts\"],"
           "\"transport\":\"spp\"}",
           DEVICE_NAME, FW_VERSION, SCR_W, SCR_H);
  sendLine(line);
}

// ---------------------------------------------------------------- xử lý một dòng HLP
static void handleLine(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;  // dòng hỏng: bỏ, đồng bộ lại ở '\n' kế
  if ((doc["v"] | 0) != 1) return;
  const char* t = doc["t"] | "";

  if (strcmp(t, "ping") == 0) {  // trả lời ngay, trước mọi việc vẽ vời
    sendLine("{\"v\":1,\"t\":\"pong\"}");
    lastRx = millis();
    return;
  }
  lastRx = millis();

  if (strcmp(t, "s") == 0) {
    resetState();  // mỗi message là snapshot đầy đủ, không phải delta
    hud.nav = (doc["nav"] | 0) != 0;
    hud.spd = doc["spd"] | 0;
    hud.lim = doc["lim"] | 0;
    hud.over = (doc["over"] | 0) != 0;
    hud.trn = doc["trn"] | 0;
    hud.trn2 = doc["trn2"] | 0;
    hud.dst = doc["dst"] | -1;
    hud.ex = doc["exit"] | 0;
    asciiFold(doc["st"] | "", hud.st, sizeof(hud.st));
    asciiFold(doc["st2"] | "", hud.st2, sizeof(hud.st2));
    asciiFold(doc["eta"] | "", hud.eta, sizeof(hud.eta));
    hud.rmin = doc["rmin"] | -1;
    hud.rm = doc["rm"] | -1;
    hud.rkm = doc["rkm"] | -1.0f;
    hud.avg = (doc["avg"] | 0) != 0;
    hud.avgL = doc["avgL"] | 0;
    hud.alr = doc["alr"] | 0;
    hud.alrD = doc["alrD"] | -1;
    hud.alrV = doc["alrV"] | 0;
  } else if (strcmp(t, "hi") == 0) {
    long sess = doc["sess"] | -1L;
    if (sess != lastSess) {  // phiên mới: xóa state cũ
      lastSess = sess;
      resetState();
    }
  } else if (strcmp(t, "bye") == 0) {
    resetState();
  }
}

static void pumpBluetooth() {
  while (SerialBT.available()) {
    int c = SerialBT.read();
    if (c < 0) break;
    if (c == '\n') {
      if (!rxOverflow && rxLen > 0) {
        if (rxBuf[rxLen - 1] == '\r') rxLen--;
        rxBuf[rxLen] = 0;
        if (rxLen > 0) handleLine(rxBuf);
      }
      rxLen = 0;
      rxOverflow = false;
    } else if (!rxOverflow) {
      if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = (char)c;
      else rxOverflow = true;  // dòng quá dài: bỏ cho tới '\n' kế tiếp
    }
  }
}

// ---------------------------------------------------------------- hàm vẽ cơ bản
static void thickLine(int x0, int y0, int x1, int y1, int w, uint16_t col) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) return;
  float nx = -dy / len * (w / 2.0f), ny = dx / len * (w / 2.0f);
  int ax = (int)(x0 + nx), ay = (int)(y0 + ny);
  int bx = (int)(x0 - nx), by = (int)(y0 - ny);
  int cx = (int)(x1 + nx), cy = (int)(y1 + ny);
  int ex = (int)(x1 - nx), ey = (int)(y1 - ny);
  tft.fillTriangle(ax, ay, bx, by, cx, cy, col);
  tft.fillTriangle(bx, by, ex, ey, cx, cy, col);
  tft.fillCircle(x0, y0, w / 2, col);
  tft.fillCircle(x1, y1, w / 2, col);
}

// Đầu mũi tên: đỉnh (tx,ty), hướng (dx,dy) chưa chuẩn hóa.
static void arrowHead(int tx, int ty, float dx, float dy, int size, uint16_t col) {
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  dx /= len;
  dy /= len;
  float bx = tx - dx * size, by = ty - dy * size;
  float px = -dy * size * 0.8f, py = dx * size * 0.8f;
  tft.fillTriangle(tx, ty, (int)(bx + px), (int)(by + py), (int)(bx - px), (int)(by - py), col);
}

// Vẽ chuỗi đoạn thẳng rồi gắn đầu mũi tên ở điểm cuối.
static void polyArrow(const int (*pts)[2], int n, int w, int head, uint16_t col) {
  for (int i = 0; i + 1 < n; i++) thickLine(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w, col);
  int dx = pts[n - 1][0] - pts[n - 2][0];
  int dy = pts[n - 1][1] - pts[n - 2][1];
  arrowHead(pts[n - 1][0], pts[n - 1][1], (float)dx, (float)dy, head, col);
}

// Vẽ biểu tượng hướng rẽ trong ô vuông tâm (cx,cy), nửa cạnh r.
static void drawTurn(int trn, int exitNo, int cx, int cy, int r, uint16_t col) {
  int w = r / 4 + 2;
  int head = r / 2 + 2;
  switch (trn) {
    case 1:  // đi thẳng
    case 19: {
      int p[][2] = {{cx, cy + r}, {cx, cy - r}};
      polyArrow(p, 2, w, head, col);
      break;
    }
    case 2:
    case 3: {  // rẽ trái / phải
      int s = (trn == 2) ? -1 : 1;
      int x0 = cx - s * r / 2;
      int p[][2] = {{x0, cy + r}, {x0, cy - r / 4}, {cx + s * r, cy - r / 4}};
      polyArrow(p, 3, w, head, col);
      break;
    }
    case 6:
    case 7: {  // rẽ gắt
      int s = (trn == 6) ? -1 : 1;
      int p[][2] = {{cx - s * r / 3, cy + r}, {cx - s * r / 3, cy - r / 3},
                    {cx + s * (r * 3 / 4), cy + r / 2}};
      polyArrow(p, 3, w, head, col);
      break;
    }
    case 4:
    case 5:
    case 13:
    case 14:
    case 15:
    case 16: {  // chếch / giữ / ra nhánh
      int s = (trn == 4 || trn == 13 || trn == 15) ? -1 : 1;
      int p[][2] = {{cx - s * r / 3, cy + r}, {cx - s * r / 3, cy + r / 4},
                    {cx + s * (r * 2 / 3), cy - r}};
      polyArrow(p, 3, w, head, col);
      break;
    }
    case 8:
    case 9: {  // quay đầu
      int s = (trn == 8) ? -1 : 1;
      int p[][2] = {{cx - s * r / 2, cy + r}, {cx - s * r / 2, cy - r * 2 / 5},
                    {cx - s * r / 3, cy - r * 3 / 4}, {cx, cy - r * 9 / 10},
                    {cx + s * r / 3, cy - r * 3 / 4}, {cx + s * r / 2, cy - r * 2 / 5},
                    {cx + s * r / 2, cy + r * 2 / 3}};
      polyArrow(p, 7, w, head, col);
      break;
    }
    case 10:
    case 11:
    case 12:
    case 20: {  // vòng xuyến
      int rcx = cx, rcy = cy - r / 8, rr = r / 2 + 2;
      tft.fillCircle(rcx, rcy, rr, col);
      tft.fillCircle(rcx, rcy, rr - w + 1, COL_BG);
      thickLine(cx, cy + r, cx, rcy + rr - 1, w, col);  // lối vào
      if (trn == 12) {
        int p[][2] = {{rcx + rr - 2, rcy - rr / 3}, {cx + r, cy - r / 2}};
        polyArrow(p, 2, w, head, col);
      } else if (trn == 11) {
        int p[][2] = {{rcx - rr + 2, rcy - rr / 3}, {cx - r, cy - r / 2}};
        polyArrow(p, 2, w, head, col);
      } else if (trn == 20) {
        int p[][2] = {{rcx - rr / 2, rcy + rr - 2}, {cx - r / 2, cy + r}};
        polyArrow(p, 2, w, head, col);
      } else {
        int p[][2] = {{rcx, rcy - rr + 2}, {cx, cy - r}};
        polyArrow(p, 2, w, head, col);
      }
      if (exitNo > 0 && exitNo < 10 && r >= 24) {
        char b[4];
        snprintf(b, sizeof(b), "%d", exitNo);
        tft.setTextColor(col, COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(b, rcx, rcy, 2);
      }
      break;
    }
    case 17: {  // đến nơi
      tft.fillCircle(cx, cy, r * 2 / 3, col);
      tft.fillCircle(cx, cy, r / 3, COL_BG);
      break;
    }
    default:
      break;  // mã lạ hoặc 0: không vẽ gì
  }
}

// ---------------------------------------------------------------- văn bản cảnh báo
static const char* alertName(int a) {
  switch (a) {
    case 1: return "CANH SAT";
    case 2: return "CAMERA TOC DO";
    case 3: return "CAMERA DEN DO";
    case 4: return "NGUY HIEM";
    case 5: return "TAI NAN";
    case 6: return "KET XE";
    case 7: return "DUONG DONG";
    case 8: return "GIAM TOC";
    case 9: return "CAM VUOT";
    case 10: return "HET CAM VUOT";
    case 11: return "DUONG SAT";
    case 12: return "TRAM THU PHI";
    case 13: return "XE DUNG";
    case 14: return "THI CONG";
    case 15: return "O GA";
    case 22: return "HET HAN CHE TOC DO";
    case 56: return "GO GIAM TOC";
    case 57: return "KHU TRUONG HOC";
    case 75: return "DEN GIAO THONG";
    default: break;
  }
  if (a >= 40 && a <= 46) return "CAMERA";
  return "CANH BAO";
}

static bool alertIsRed(int a) { return a == 1 || a == 2 || a == 3 || (a >= 40 && a <= 46); }

// ---------------------------------------------------------------- các vùng màn hình
static LinkState linkState() {
  if (!wasUp) return LINK_WAIT;
  return (millis() - lastRx > 3000) ? LINK_STALE : LINK_LIVE;
}

static bool isOver() { return hud.over || (hud.lim > 0 && hud.spd > hud.lim); }

static void drawWait() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString("WazeHUD", SCR_W / 2, 70, 4);
  tft.setTextColor(COL_WARN, COL_BG);
  tft.drawString("Cho ket noi...", SCR_W / 2, 120, 4);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("1. Ghep noi \"" DEVICE_NAME "\" trong Bluetooth", SCR_W / 2, 165, 2);
  tft.drawString("2. Waze Mod > HUD Link > chon thiet bi", SCR_W / 2, 185, 2);
  tft.drawString("Firmware " FW_VERSION, SCR_W / 2, 222, 2);
}

static void drawTop(LinkState ls) {
  tft.fillRect(0, 0, SCR_W, TOP_H, COL_BG);
  bool alertShown = (hud.alr != 0) || hud.avg;
  if (alertShown && ls != LINK_STALE) {
    char txt[48];
    uint16_t bg;
    uint16_t fg;
    if (hud.alr != 0) {
      const char* name = alertName(hud.alr);
      char dist[16] = "";
      if (hud.alrD >= 0) {
        char n[12];
        const char* u;
        fmtDist(hud.alrD, n, sizeof(n), &u);
        snprintf(dist, sizeof(dist), " %s%s", n, u);
      }
      if ((hud.alr == 8 || hud.alr == 22) && hud.alrV > 0)
        snprintf(txt, sizeof(txt), "%s %d%s", name, hud.alrV, dist);
      else
        snprintf(txt, sizeof(txt), "%s%s", name, dist);
      bg = alertIsRed(hud.alr) ? COL_BAD : COL_AMBER;
    } else {
      if (hud.avgL > 0) snprintf(txt, sizeof(txt), "KHU CAM VUOT %dm", hud.avgL);
      else snprintf(txt, sizeof(txt), "KHU CAM VUOT");
      bg = COL_AMBER;
    }
    fg = (bg == COL_BAD) ? TFT_WHITE : TFT_BLACK;
    tft.fillRect(0, 0, SCR_W, TOP_H, bg);
    int font = (tft.textWidth(txt, 4) <= SCR_W - 8) ? 4 : 2;
    tft.setTextColor(fg, bg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(txt, SCR_W / 2, TOP_H / 2, font);
    return;
  }

  uint16_t dot = (ls == LINK_LIVE) ? COL_OK : (ls == LINK_STALE ? COL_WARN : COL_BAD);
  tft.fillCircle(12, TOP_H / 2, 6, dot);
  tft.setTextDatum(ML_DATUM);
  if (ls == LINK_STALE) {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("MAT TIN HIEU", 26, TOP_H / 2, 2);
    return;
  }
  if (!hud.nav) return;

  char left[24] = "";
  if (hud.eta[0]) snprintf(left, sizeof(left), "ETA %s", hud.eta);
  tft.setTextColor(COL_FG, COL_BG);
  if (left[0]) tft.drawString(left, 26, TOP_H / 2, 2);

  char right[40] = "";
  char rem[16] = "";
  if (hud.rm >= 0) {
    char n[12];
    const char* u;
    fmtDist(hud.rm, n, sizeof(n), &u);
    snprintf(rem, sizeof(rem), "%s%s", n, u);
  } else if (hud.rkm >= 0) {
    snprintf(rem, sizeof(rem), "%.1fkm", hud.rkm);
  }
  if (hud.rmin >= 0 && rem[0]) snprintf(right, sizeof(right), "%d phut  %s", hud.rmin, rem);
  else if (hud.rmin >= 0) snprintf(right, sizeof(right), "%d phut", hud.rmin);
  else snprintf(right, sizeof(right), "%s", rem);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(right, SCR_W - 6, TOP_H / 2, 2);
}

static void drawSpeed(LinkState ls) {
  tft.fillRect(0, SPD_Y, SPD_AREA_W, LOW_Y - SPD_Y - 2, COL_BG);
  char b[8];
  snprintf(b, sizeof(b), "%d", hud.spd);
  uint16_t col = (ls == LINK_STALE) ? COL_DIM : (isOver() ? COL_BAD : COL_FG);
  int font = (tft.textWidth(b, 8) <= SPD_AREA_W - 8) ? 8 : 6;
  tft.setTextColor(col, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(b, SPD_AREA_W / 2, SPD_Y, font);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("km/h", SPD_AREA_W / 2, 110, 2);
}

static void drawLimit(LinkState ls) {
  tft.fillRect(SIGN_CX - SIGN_R - 2, SIGN_CY - SIGN_R - 2, 2 * SIGN_R + 4, 2 * SIGN_R + 4, COL_BG);
  if (hud.lim > 0) {
    uint16_t ring = (ls == LINK_STALE) ? COL_DIM : COL_BAD;
    tft.fillCircle(SIGN_CX, SIGN_CY, SIGN_R, ring);
    tft.fillCircle(SIGN_CX, SIGN_CY, SIGN_R - 9, TFT_WHITE);
    char b[8];
    snprintf(b, sizeof(b), "%d", hud.lim);
    int inner = (SIGN_R - 9) * 2 - 10;
    int font = (tft.textWidth(b, 6) <= inner) ? 6 : 4;
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(b, SIGN_CX, SIGN_CY, font);
  } else {
    tft.fillCircle(SIGN_CX, SIGN_CY, SIGN_R, 0x2104);
    tft.fillCircle(SIGN_CX, SIGN_CY, SIGN_R - 4, COL_BG);
  }
}

static void drawTurnArea(LinkState ls) {
  tft.fillRect(0, LOW_Y, SCR_W, ST_Y - LOW_Y, COL_BG);
  if (ls == LINK_STALE || !hud.nav || hud.trn == 0) return;
  uint16_t col = COL_ARROW;
  drawTurn(hud.trn, hud.ex, 52, 172, 32, col);
  if (hud.dst >= 0) {
    char n[12];
    const char* u;
    fmtDist(hud.dst, n, sizeof(n), &u);
    tft.setTextColor(COL_FG, COL_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(n, 112, 172, 6);
    int tw = tft.textWidth(n, 6);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(u, 112 + tw + 6, 180, 4);
  }
  if (hud.trn2 != 0) drawTurn(hud.trn2, 0, 288, 172, 14, COL_DIM);
}

static void currentStreet(char* out, size_t cap) {
  const char* s = (hud.nav && hud.trn != 0 && hud.st2[0]) ? hud.st2 : hud.st;
  snprintf(out, cap, "%s", s);
}

static void drawStreet(LinkState ls) {
  tft.fillRect(0, ST_Y, SCR_W, SCR_H - ST_Y, COL_BG);
  if (ls == LINK_STALE) return;
  char s[48];
  currentStreet(s, sizeof(s));
  if (!s[0]) return;
  size_t len = strlen(s);
  while (len > 3 && tft.textWidth(s, 4) > SCR_W - 8) {  // cắt ngắn cho vừa màn hình
    len--;
    s[len] = 0;
    if (len >= 2) s[len - 1] = '.', s[len - 2] = '.';
  }
  tft.setTextColor(COL_FG, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(s, SCR_W / 2, ST_Y + (SCR_H - ST_Y) / 2, 4);
}

// ---------------------------------------------------------------- vẽ lại chỉ phần đã đổi
static uint32_t hMix(uint32_t h, uint32_t v) { return h ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2)); }
static uint32_t hStr(uint32_t h, const char* s) {
  while (*s) h = hMix(h, (uint8_t)*s++);
  return h;
}

static uint8_t drawnMode = 255;
static uint32_t hTop = 0, hSpd = 0, hLim = 0, hTurn = 0, hSt = 0;
static bool needFull = true;

static void render() {
  LinkState ls = linkState();
  uint8_t mode = (ls == LINK_WAIT) ? 0 : 1;
  if (mode != drawnMode) {
    drawnMode = mode;
    needFull = true;
    tft.fillScreen(COL_BG);
  }
  if (mode == 0) {
    if (needFull) drawWait();
    needFull = false;
    return;
  }

  uint32_t t = hMix(1, ls);
  t = hMix(t, hud.nav);
  t = hStr(t, hud.eta);
  t = hMix(t, (uint32_t)hud.rmin);
  t = hMix(t, (uint32_t)hud.rm);
  t = hMix(t, (uint32_t)(hud.rkm * 10));
  t = hMix(t, (uint32_t)hud.alr);
  t = hMix(t, (uint32_t)hud.alrD);
  t = hMix(t, (uint32_t)hud.alrV);
  t = hMix(t, hud.avg);
  t = hMix(t, (uint32_t)hud.avgL);
  if (needFull || t != hTop) {
    hTop = t;
    drawTop(ls);
  }

  uint32_t sp = hMix(2, (uint32_t)hud.spd);
  sp = hMix(sp, isOver());
  sp = hMix(sp, ls);
  if (needFull || sp != hSpd) {
    hSpd = sp;
    drawSpeed(ls);
  }

  uint32_t lm = hMix(3, (uint32_t)hud.lim);
  lm = hMix(lm, ls);
  if (needFull || lm != hLim) {
    hLim = lm;
    drawLimit(ls);
  }

  char dn[12] = "";
  const char* du = "";
  if (hud.dst >= 0) fmtDist(hud.dst, dn, sizeof(dn), &du);
  uint32_t tn = hMix(4, (uint32_t)hud.trn);
  tn = hMix(tn, (uint32_t)hud.trn2);
  tn = hMix(tn, (uint32_t)hud.ex);
  tn = hMix(tn, hud.nav);
  tn = hMix(tn, ls);
  tn = hStr(hStr(tn, dn), du);
  if (needFull || tn != hTurn) {
    hTurn = tn;
    drawTurnArea(ls);
  }

  char street[48];
  currentStreet(street, sizeof(street));
  uint32_t st = hStr(hMix(5, ls), street);
  if (needFull || st != hSt) {
    hSt = st;
    drawStreet(ls);
  }
  needFull = false;
}

// ---------------------------------------------------------------- setup / loop
static void onConnect() {
  rxLen = 0;
  rxOverflow = false;
  lastSess = -1;
  resetState();
  lastRx = millis();
  sendDevPending = true;
  needFull = true;
  Serial.println("HLP: da ket noi");
}

static void onDisconnect() {
  resetState();
  needFull = true;
  Serial.println("HLP: mat ket noi");
}

void setup() {
  Serial.begin(115200);
  // Tắt đèn LED RGB trên board CYD (chân 4/16/17, tích cực mức thấp).
  const int leds[] = {4, 16, 17};
  for (int i = 0; i < 3; i++) {
    pinMode(leds[i], OUTPUT);
    digitalWrite(leds[i], HIGH);
  }
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);  // đèn nền

  tft.init();
  tft.setRotation(HUD_ROTATION);
  tft.fillScreen(COL_BG);
  resetState();
  drawWait();

  SerialBT.begin(DEVICE_NAME);
  Serial.println("WazeHUD " FW_VERSION " san sang, cho Waze Mod ket noi...");
}

void loop() {
  bool up = SerialBT.hasClient();
  if (up && !wasUp) {
    wasUp = true;
    onConnect();
  } else if (!up && wasUp) {
    wasUp = false;
    onDisconnect();
  }

  if (wasUp) {
    if (sendDevPending) {
      sendDevPending = false;
      sendDev();
    }
    pumpBluetooth();
  }

  static uint32_t lastRender = 0;
  if (millis() - lastRender >= 50) {
    lastRender = millis();
    render();
  }
  delay(2);
}
