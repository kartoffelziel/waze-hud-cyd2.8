#pragma once
// Hàm tiện ích thuần C++ (không phụ thuộc Arduino) để dễ kiểm thử trên máy tính.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Bảng đổi chữ Việt Nam (U+1EA0..U+1EF9, 90 ký tự, chẵn = hoa, lẻ = thường) sang ASCII.
static const char VN_FOLD[] =
    "AaAaAaAaAaAaAaAaAaAaAaAa"  // Ạ..ặ (24)
    "EeEeEeEeEeEeEeEe"          // Ẹ..ệ (16)
    "IiIi"                      // Ỉ..ị (4)
    "OoOoOoOoOoOoOoOoOoOoOoOo"  // Ọ..ợ (24)
    "UuUuUuUuUuUuUu"            // Ụ..ự (14)
    "YyYyYyYy";                 // Ỳ..ỹ (8)

// Trả về ký tự ASCII thay thế; 0 = bỏ qua ký tự này.
static inline char foldCodepoint(uint32_t c) {
  if (c < 0x20) return ' ';
  if (c < 0x80) return (char)c;
  if (c >= 0x300 && c <= 0x36F) return 0;  // dấu kết hợp
  if (c >= 0x1EA0 && c <= 0x1EF9) return VN_FOLD[c - 0x1EA0];
  switch (c) {
    case 0xA0: return ' ';
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0x102: return 'A';
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0x103: return 'a';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: case 0x128: return 'I';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0x129: return 'i';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0x1A0: return 'O';
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0x1A1: return 'o';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0x168: case 0x1AF: return 'U';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0x169: case 0x1B0: return 'u';
    case 0xDD: return 'Y';
    case 0xFD: case 0xFF: return 'y';
    case 0xD0: case 0x110: return 'D';
    case 0x111: return 'd';
    default: return '?';
  }
}

// Đổi chuỗi UTF-8 sang ASCII (bỏ dấu). Trả về độ dài chuỗi ra.
static inline size_t asciiFold(const char* in, char* out, size_t cap) {
  size_t n = 0;
  if (!out || cap == 0) return 0;
  const uint8_t* p = (const uint8_t*)(in ? in : "");
  while (*p && n + 1 < cap) {
    uint32_t c = *p;
    int extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { p++; continue; }  // byte tiếp nối lạc chỗ
    p++;
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) { c = '?'; break; }
      c = (c << 6) | (*p & 0x3F);
      p++;
    }
    char f = foldCodepoint(c);
    if (f) out[n++] = f;
  }
  out[n] = 0;
  return n;
}

// Định dạng khoảng cách: <1000 m -> "250" + "m"; >=1000 m -> "1.2" + "km".
static inline void fmtDist(int meters, char* num, size_t cap, const char** unit) {
  if (meters < 0) meters = 0;
  if (meters >= 1000) {
    int tenths = (meters + 50) / 100;  // làm tròn 0,1 km
    snprintf(num, cap, "%d.%d", tenths / 10, tenths % 10);
    *unit = "km";
  } else {
    if (meters >= 100) meters = ((meters + 5) / 10) * 10;  // làm tròn 10 m
    if (meters >= 1000) {  // 995..999 m làm tròn thành 1000
      snprintf(num, cap, "1.0");
      *unit = "km";
      return;
    }
    snprintf(num, cap, "%d", meters);
    *unit = "m";
  }
}
