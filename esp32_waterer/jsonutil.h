#pragma once
/*
 * jsonutil.h —— 极简 JSON 取值（扁平 key，无嵌套遍历）。
 *
 * 只支持本项目用到的几种取值：整型、布尔、字符串。
 * 放在公共头里，web_server 与 mqtt_link 共用，避免两边各写一份。
 * 用 static inline，被多个 .cpp 包含也不会出现多重定义。
 */
#include <Arduino.h>

static inline long jUtilInt(const String& b, const char* key, long def) {
  String pat = String("\"") + key + "\":";
  int i = b.indexOf(pat);
  if (i < 0) return def;
  int p = i + (int)pat.length();
  int len = (int)b.length();
  while (p < len && (b[p] == ' ' || b[p] == '\t')) p++;
  bool neg = false;
  if (p < len && b[p] == '-') { neg = true; p++; }
  long v = 0;
  bool any = false;
  while (p < len && b[p] >= '0' && b[p] <= '9') {
    v = v * 10 + (b[p] - '0');
    p++;
    any = true;
  }
  if (!any) return def;
  return neg ? -v : v;
}

static inline bool jUtilBool(const String& b, const char* key, bool def) {
  String t = String("\"") + key + "\":true";
  if (b.indexOf(t) >= 0) return true;
  String f = String("\"") + key + "\":false";
  if (b.indexOf(f) >= 0) return false;
  return jUtilInt(b, key, def ? 1 : 0) != 0;
}

static inline bool jUtilStr(const String& b, const char* key, char* out, size_t n) {
  String pat = String("\"") + key + "\":\"";
  int i = b.indexOf(pat);
  if (i < 0) return false;
  int s = i + (int)pat.length();
  int e = b.indexOf('"', s);
  if (e < 0) return false;
  String v = b.substring(s, e);
  strncpy(out, v.c_str(), n - 1);
  out[n - 1] = 0;
  return true;
}
