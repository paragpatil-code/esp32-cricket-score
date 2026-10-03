/*
 * ============================================================
 *  ESP32 - India Cricket Score Monitor  v2.1
 *  Display: SH1106 128x64 OLED (I2C) via U8g2
 *
 *  SCREEN ROTATION (10s each):
 *   [0] Score overview  - team, IND 323-7 (48.4), CRR, match note
 *   [1] Current batters - Name  R  B  4s  6s  SR
 *   [2] Current bowlers - Name  O  M  R  W  ECO  + recent balls
 *
 *  DATA SOURCE (cascade):
 *   Step 1: Cricinfo RSS - discover India intl match (lightweight)
 *   Step 2: Cricbuzz live-scores listing - find the match URL
 *   Step 3: Cricbuzz match page HTML - scrape full detail
 *
 *  Libraries (Arduino Library Manager):
 *   - U8g2  (by olikraus)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <U8g2lib.h>
#include <Wire.h>

// ==================== USER CONFIG ====================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// =====================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const uint32_t FETCH_INTERVAL  = 30000;
const uint32_t SCREEN_INTERVAL = 10000;
const uint32_t ANIM_INTERVAL   = 400;

uint32_t lastFetch  = 0;
uint32_t lastScreen = 0;
uint32_t lastAnim   = 0;

// CPU Calculation globals
uint32_t totalLoopTime = 0;
uint32_t busyLoopTime  = 0;
uint32_t lastCpuCalc   = 0;
int      cpuPct        = 0;

String matchTeams   = "";
String matchDesc    = "";
String matchScore   = "";
String matchStatus  = "";
String crrStr       = "";
String srcLabel     = "";

struct Batter {
  String name;
  int    runs, balls, fours, sixes;
  float  sr;
  bool   onStrike;
};
Batter batters[2];
int    batterCount = 0;

struct Bowler {
  String name;
  String overs;
  int    maidens, runs, wickets;
  float  economy;
  bool   bowling;
};
Bowler bowlers[2];
int    bowlerCount = 0;

String recentBalls  = "";
bool   matchFound   = false;
bool   fetching     = false;
int    currentScreen = 0;
uint8_t dotCount    = 0;

const char* INTL_TEAMS[] = {
  "Australia","England","South Africa","New Zealand",
  "Pakistan","Sri Lanka","West Indies","Bangladesh",
  "Afghanistan","Zimbabwe","Ireland","Scotland",
  "Netherlands","Namibia","UAE","Nepal","Oman",
  "Papua New Guinea","USA","Canada"
};
const int INTL_TEAM_COUNT = sizeof(INTL_TEAMS) / sizeof(INTL_TEAMS[0]);

// =====================================================
//  Helpers
// =====================================================

bool containsIC(const String& h, const char* n) {
  String a = h, b = n;
  a.toLowerCase(); b.toLowerCase();
  return a.indexOf(b) >= 0;
}

bool isInternational(const String& title) {
  if (!containsIC(title, "India")) return false;
  for (int i = 0; i < INTL_TEAM_COUNT; i++)
    if (containsIC(title, INTL_TEAMS[i])) return true;
  return false;
}

String decodeEntities(String s) {
  s.replace("&amp;","&"); s.replace("&lt;","<");
  s.replace("&gt;",">"); s.replace("&quot;","\"");
  s.replace("&nbsp;"," "); s.replace("&#x27;","'");
  return s;
}

String stripTags(String s) {
  while (true) {
    int a = s.indexOf('<');
    if (a < 0) break;
    int b = s.indexOf('>', a);
    if (b < 0) break;
    s = s.substring(0, a) + s.substring(b + 1);
  }
  return s;
}

// "Alzarri Joseph" -> "A.Joseph" (max maxLen chars)
String shortName(const String& full, int maxLen) {
  String s = full;
  s.trim();
  if ((int)s.length() <= maxLen) return s;
  int sp = s.indexOf(' ');
  if (sp > 0) {
    String abbr = String(s[0]) + "." + s.substring(sp + 1);
    if ((int)abbr.length() <= maxLen) return abbr;
    return abbr.substring(0, maxLen);
  }
  return s.substring(0, maxLen);
}

// =====================================================
//  Cricbuzz HTML parser  (v2.1 - fixed)
// =====================================================

void resetMatchData() {
  matchTeams=""; matchDesc=""; matchScore="";
  matchStatus=""; crrStr=""; recentBalls="";
  batterCount = 0; bowlerCount = 0;
  for (int i = 0; i < 2; i++) {
    batters[i] = {"",0,0,0,0,0.0f,false};
    bowlers[i] = {"","",0,0,0,0.0f,false};
  }
}

bool parseCricbuzzHTML(const String& html) {
  resetMatchData();

  // ------------------------------------------------------------------
  // SCORE: Extract from <meta property="og:title" ...>
  // Cricbuzz embeds the clean score in the opengraph title!
  // e.g., content="WI 56/1 (7.1) vs IND 351/7 (Amir Jangoo 9(8)...) | India vs..."
  // ------------------------------------------------------------------
  {
    int og = html.indexOf("og:title\" content=\"");
    if (og > 0) {
      int s = og + 19;
      int e = html.indexOf('"', s);
      if (e > s) {
        String ogTitle = html.substring(s, e);
        
        // Remove everything after '|'
        int pipe = ogTitle.indexOf('|');
        if (pipe > 0) ogTitle = ogTitle.substring(0, pipe);
        
        // Clean whitespace
        ogTitle.replace("\n", " ");
        while (ogTitle.indexOf("  ") >= 0) ogTitle.replace("  ", " ");
        ogTitle.trim();
        
        // Remove "Follow " if present (from og:description)
        if (ogTitle.startsWith("Follow ")) ogTitle = ogTitle.substring(7);
        
        // Remove trailing batter stats e.g. "(Amir Jangoo 9(8) Shai Hope 31(28))"
        // We know it's a batter stat block if the last '(' contains letters (names).
        // Overs like "(48.4)" only contain digits and dots.
        int lastOpen = ogTitle.lastIndexOf('(');
        if (lastOpen > 0) {
          bool hasLetter = false;
          for (int i = lastOpen; i < (int)ogTitle.length(); i++) {
            if (isAlpha(ogTitle[i])) { hasLetter = true; break; }
          }
          if (hasLetter) {
            ogTitle = ogTitle.substring(0, lastOpen);
            ogTitle.trim();
          }
        }
        
        if (ogTitle.length() > 3) {
          matchScore = ogTitle;
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // CRR
  // Pattern in HTML: <span class="...">CRR:</span><span class="...">6.64</span>
  // ------------------------------------------------------------------
  {
    int crrPos = html.indexOf("CRR:</span>");
    if (crrPos < 0) crrPos = html.indexOf(">CRR<");
    if (crrPos >= 0) {
      int gt = html.indexOf('>', crrPos + 3);
      if (gt >= 0) {
        int lt = html.indexOf('<', gt + 1);
        if (lt > gt) {
          crrStr = html.substring(gt + 1, lt);
          crrStr.trim();
        }
      }
    }
    // Fallback: find numeric value near "CRR" text node
    if (crrStr.length() == 0 || crrStr.length() > 6) {
      crrStr = "";
      int cp2 = html.indexOf("CRR");
      while (cp2 >= 0) {
        String ctx = html.substring(cp2, min((int)html.length(), cp2 + 120));
        ctx = stripTags(ctx);
        ctx.replace("CRR",""); ctx.replace(":",""); ctx.trim();
        int st = 0;
        while (st < (int)ctx.length() && !isDigit(ctx[st])) st++;
        if (st < (int)ctx.length()) {
          int en = st;
          while (en < (int)ctx.length() && (isDigit(ctx[en]) || ctx[en]=='.')) en++;
          String cand = ctx.substring(st, en);
          if (cand.length() > 0 && cand.length() < 6) { crrStr = cand; break; }
        }
        cp2 = html.indexOf("CRR", cp2 + 3);
      }
    }
  }

  // ------------------------------------------------------------------
  // MATCH STATUS (toss note / "opt to bat" / "Stumps" etc.)
  // ------------------------------------------------------------------
  {
    const char* pats[] = {"text-cbLive\">","text-cbTxtLive\">","cbLiveStatus\">"};
    for (const char* pat : pats) {
      int sp = html.indexOf(pat);
      if (sp >= 0) {
        int gt = html.indexOf('>', sp + strlen(pat) - 2);
        if (gt >= 0) {
          int lt = html.indexOf('<', gt + 1);
          if (lt > gt) {
            String st = html.substring(gt + 1, lt);
            st = decodeEntities(st); st.trim();
            if (st.length() > 2) { matchStatus = st; break; }
          }
        }
      }
    }
    if (matchStatus.length() == 0) {
      const char* phrases[] = {"opt to bat","opt to bowl","won by","Stumps","Lunch","Tea","Rain","Innings Break"};
      for (const char* ph : phrases) {
        int pp = html.indexOf(ph);
        if (pp >= 0) {
          String chunk = html.substring(max(0,pp-15), min((int)html.length(),pp+(int)strlen(ph)+20));
          chunk = stripTags(chunk); chunk = decodeEntities(chunk); chunk.trim();
          if (chunk.length() > 2) { matchStatus = chunk; break; }
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // MATCH DESCRIPTION from <h1>
  // ------------------------------------------------------------------
  {
    int h1s = html.indexOf("<h1 ");
    if (h1s >= 0) {
      int h1e = html.indexOf("</h1>", h1s);
      if (h1e > h1s) {
        String h1 = stripTags(html.substring(h1s, h1e));
        h1 = decodeEntities(h1); h1.trim();
        int c1 = h1.indexOf(',');
        if (c1 > 0) {
          matchTeams = h1.substring(0, c1); matchTeams.trim();
          int c2 = h1.indexOf(',', c1 + 1);
          matchDesc = h1.substring(c1 + 1, (c2 > 0 ? c2 : (int)h1.length()));
          matchDesc.trim();
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // REQUIRED RUN RATE (RRR)
  // If we are in the 2nd innings, Cricbuzz shows REQ. We can display this
  // instead of the matchDesc (e.g. instead of "3rd ODI").
  // Pattern: REQ:</span><span class="...">6.83</span>
  // ------------------------------------------------------------------
  {
    int reqPos = html.indexOf("REQ:</span>");
    if (reqPos < 0) reqPos = html.indexOf("REQ:"); // fallback
    if (reqPos >= 0) {
      String reqCtx = html.substring(reqPos, min((int)html.length(), reqPos + 80));
      reqCtx = stripTags(reqCtx);
      reqCtx.replace("REQ:",""); reqCtx.trim();
      int st = 0;
      while (st < (int)reqCtx.length() && !isDigit(reqCtx[st])) st++;
      if (st < (int)reqCtx.length()) {
        int en = st;
        while (en < (int)reqCtx.length() && (isDigit(reqCtx[en]) || reqCtx[en]=='.')) en++;
        String cand = reqCtx.substring(st, en);
        if (cand.length() > 0 && cand.length() < 6) {
          matchDesc = "RRR:" + cand; // Override the "3rd ODI" text
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // BATTERS
  // Each batter row is a div with class containing "scorecard-bat-grid".
  // Within it:
  //   Name: <span class="hover:underline">KL Rahul<span> * </span></span>
  //   Stats: 5x divs with class "flex justify-center items-center text-sm"
  //          -> R, B, 4s, 6s, SR
  // ------------------------------------------------------------------
  {
    const char* GRID = "class=\"grid scorecard-bat-grid";
    int pos = 0;
    int parsed = 0;

    int headerPos = html.indexOf(">Batter<");
    if (headerPos > 0) pos = headerPos;

    int bowlerHeaderPos = html.indexOf(">Bowler<");

    while (parsed < 2) {
      int rowStart = html.indexOf(GRID, pos);
      if (rowStart < 0) break;
      if (bowlerHeaderPos > 0 && rowStart > bowlerHeaderPos) break;

      int nextGrid = html.indexOf(GRID, rowStart + strlen(GRID));
      int rowEnd   = (nextGrid > 0) ? min(nextGrid, rowStart + 4500) : rowStart + 4500;
      String row   = html.substring(rowStart, min((int)html.length(), rowEnd));

      // --- Extract player name from hover:underline span ---
      String pName  = "";
      bool   strike = false;
      int ns = row.indexOf("hover:underline");
      if (ns >= 0) {
        int gt = row.indexOf('>', ns);
        if (gt >= 0) {
          int lt = row.indexOf('<', gt + 1);
          if (lt > gt) {
            pName = row.substring(gt + 1, lt);
            pName.trim();
          }
          // Check next 80 chars after name for strike indicator "*"
          String afterName = row.substring(lt, min((int)row.length(), lt + 80));
          if (afterName.indexOf("* ") >= 0 || afterName.indexOf("*<") >= 0) {
            strike = true;
          }
          if (!strike) {
            int spanPos = row.indexOf("<span>", lt);
            if (spanPos >= 0 && spanPos < lt + 80) {
              int sgt = row.indexOf('>', spanPos);
              int slt = row.indexOf('<', sgt + 1);
              if (sgt >= 0 && slt > sgt && row.substring(sgt+1,slt).indexOf('*') >= 0)
                strike = true;
            }
          }
        }
      }

      if (pName.length() < 2 || pName.indexOf("Batter") >= 0 || pName.indexOf("Bowler") >= 0) {
        pos = rowStart + strlen(GRID); continue;
      }

      // --- Extract 5 stat cells: R B 4s 6s SR ---
      const char* CELL = "justify-center items-center text-sm";
      int cp = 0;
      String cells[5];
      for (int ci = 0; ci < 5; ci++) {
        int cs = row.indexOf(CELL, cp);
        if (cs < 0) break;
        int gt = row.indexOf('>', cs);
        if (gt < 0) break;
        int lt = row.indexOf('<', gt + 1);
        if (lt < 0) break;
        cells[ci] = row.substring(gt + 1, lt);
        cells[ci].trim();
        cp = lt;
      }

      batters[parsed] = {pName, cells[0].toInt(), cells[1].toInt(),
                         cells[2].toInt(), cells[3].toInt(),
                         cells[4].toFloat(), strike};
      parsed++;
      pos = rowStart + strlen(GRID);
    }
    batterCount = parsed;
  }

  // ------------------------------------------------------------------
  // BOWLERS (same grid class, after ">Bowler<" header)
  // ------------------------------------------------------------------
  {
    const char* GRID = "class=\"grid scorecard-bat-grid";
    int parsed = 0;

    int bowlerHeaderPos = html.indexOf(">Bowler<");
    int pos = (bowlerHeaderPos > 0) ? bowlerHeaderPos : (int)(html.length() / 2);

    while (parsed < 2) {
      int rowStart = html.indexOf(GRID, pos);
      if (rowStart < 0) break;

      int nextGrid = html.indexOf(GRID, rowStart + strlen(GRID));
      int rowEnd   = (nextGrid > 0) ? min(nextGrid, rowStart + 4500) : rowStart + 4500;
      String row   = html.substring(rowStart, min((int)html.length(), rowEnd));

      String bName  = "";
      bool   bowling = false;
      int ns = row.indexOf("hover:underline");
      if (ns >= 0) {
        int gt = row.indexOf('>', ns);
        if (gt >= 0) {
          int lt = row.indexOf('<', gt + 1);
          if (lt > gt) {
            bName = row.substring(gt + 1, lt);
            bName.trim();
          }
          String afterName = row.substring(lt, min((int)row.length(), lt + 80));
          if (afterName.indexOf("* ") >= 0 || afterName.indexOf("*<") >= 0) {
            bowling = true;
          }
          if (!bowling) {
            int spanPos = row.indexOf("<span>", lt);
            if (spanPos >= 0 && spanPos < lt + 80) {
              int sgt = row.indexOf('>', spanPos);
              int slt = row.indexOf('<', sgt + 1);
              if (sgt >= 0 && slt > sgt && row.substring(sgt+1,slt).indexOf('*') >= 0)
                bowling = true;
            }
          }
        }
      }

      if (bName.length() < 2 || bName.indexOf("Batter") >= 0 || bName.indexOf("Bowler") >= 0) {
        pos = rowStart + strlen(GRID); continue;
      }

      const char* CELL = "justify-center items-center text-sm";
      int cp = 0;
      String cells[5];
      for (int ci = 0; ci < 5; ci++) {
        int cs = row.indexOf(CELL, cp);
        if (cs < 0) break;
        int gt = row.indexOf('>', cs);
        if (gt < 0) break;
        int lt = row.indexOf('<', gt + 1);
        if (lt < 0) break;
        cells[ci] = row.substring(gt + 1, lt);
        cells[ci].trim();
        cp = lt;
      }

      bowlers[parsed] = {bName, cells[0], cells[1].toInt(),
                         cells[2].toInt(), cells[3].toInt(),
                         cells[4].toFloat(), bowling};
      parsed++;
      pos = rowStart + strlen(GRID);
    }
    bowlerCount = parsed;
  }

  // ------------------------------------------------------------------
  // RECENT BALLS
  // Cricbuzz HTML: "Recent :" followed by individual <span>0</span> etc.
  // Walk char-by-char collecting ONLY digit/W/b characters from text nodes.
  // This avoids picking up raw HTML tags if stripTags() doesn't catch them.
  // ------------------------------------------------------------------
  {
    const char* recPat = "Recent :";
    int rp = html.indexOf(recPat);
    if (rp < 0) rp = html.indexOf("Recent:");
    if (rp >= 0) {
      String balls = "";
      bool inTag2  = false;
      int  limit   = min((int)html.length(), rp + 300);
      for (int ci = rp + 8; ci < limit && balls.length() < 22; ci++) {
        char c = html[ci];
        if (c == '<') {
          inTag2 = true;
          // Stop if we hit the end of the containing div
          String ahead = html.substring(ci, min((int)html.length(), ci + 6));
          if (ahead.startsWith("</div") || ahead.startsWith("</p")) {
            if (balls.length() > 0) break;
          }
        } else if (c == '>') {
          inTag2 = false;
        } else if (!inTag2) {
          // Accept digits, W (wide), w, Nb (no-ball), +, space
          if (isDigit(c) || c == 'W' || c == 'w' || c == 'b' ||
              c == '+' || c == 'N' || c == ' ') {
            if (!(c == ' ' && balls.length() == 0)) // no leading space
              balls += c;
          }
        }
      }
      balls.trim();
      while (balls.indexOf("  ") >= 0) balls.replace("  "," ");
      if (balls.length() > 1) recentBalls = balls;
    }
  }

  return (matchScore.length() > 0 || batterCount > 0);
}

// =====================================================
//  Find India intl match URL on Cricbuzz listing page
// =====================================================
String findIndiaCricbuzzUrl() {
  HTTPClient http;
  http.begin("https://www.cricbuzz.com/cricket-match/live-scores");
  http.setTimeout(12000);
  http.addHeader("User-Agent",
    "Mozilla/5.0 (Linux; Android 11; Pixel 5) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36");

  int code = http.GET();
  Serial.printf("[CB-list] HTTP %d\n", code);
  if (code != 200) { http.end(); return ""; }

  String body = http.getString();
  http.end();

  int intlSection     = body.indexOf("INTERNATIONAL");
  if (intlSection < 0) intlSection = 0;
  int domesticSection = body.indexOf("DOMESTIC", intlSection);
  if (domesticSection < 0) domesticSection = body.length();

  String section = body.substring(intlSection, domesticSection);

  int pos = 0;
  String firstIndiaUrl = "";

  while (true) {
    int linkStart = section.indexOf("href=\"/live-cricket-scores/", pos);
    if (linkStart < 0) break;

    int hrefStart = linkStart + 6;
    int hrefEnd   = section.indexOf('"', hrefStart);
    String href   = section.substring(hrefStart, hrefEnd);

    int titlePos = section.indexOf("title=\"", linkStart);
    int nextA    = section.indexOf("<a ", linkStart + 2);
    String titleA = "";
    if (titlePos >= 0 && (nextA < 0 || titlePos < nextA + 200)) {
      int te = section.indexOf('"', titlePos + 7);
      titleA = section.substring(titlePos + 7, te);
    }

    bool isIndia = isInternational(titleA) || isInternational(href);

    if (isIndia) {
      int chunkEnd = min((int)section.length(), (int)hrefEnd + 300);
      String nearby = section.substring(linkStart, chunkEnd);
      bool isLive = (nearby.indexOf("cbPlusLiveTag") >= 0) ||
                    (nearby.indexOf("LIVE") >= 0) ||
                    titleA.indexOf("Live") >= 0;

      String fullUrl = "https://www.cricbuzz.com" + href;
      Serial.printf("[CB-list] %s live=%d\n", href.c_str(), (int)isLive);

      if (firstIndiaUrl.length() == 0) firstIndiaUrl = fullUrl;
      if (isLive) return fullUrl;
    }
    pos = hrefEnd + 1;
  }
  return firstIndiaUrl;
}

// =====================================================
//  Fetch full match page
// =====================================================
bool fetchCricbuzzMatch(const String& url) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(15000);
  http.addHeader("User-Agent",
    "Mozilla/5.0 (Linux; Android 11; Pixel 5) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36");
  http.addHeader("Accept-Language","en-US,en;q=0.9");

  int code = http.GET();
  Serial.printf("[CB-match] HTTP %d\n", code);
  if (code != 200) { http.end(); return false; }

  String body = http.getString();
  http.end();
  Serial.printf("[CB-match] %d bytes\n", body.length());

  bool ok = parseCricbuzzHTML(body);
  if (ok) srcLabel = "cricbuzz";
  return ok;
}

// =====================================================
//  Main fetch orchestrator
// =====================================================
void fetchScore() {
  fetching   = true;
  matchFound = false;
  Serial.println("\n=== Fetching score ===");

  // Step 1: RSS quick check
  bool rssHasIndia = false;
  {
    HTTPClient http;
    http.begin("http://static.cricinfo.com/rss/livescores.xml");
    http.setTimeout(8000);
    http.addHeader("User-Agent","ESP32CricketMonitor/1.0");
    int code = http.GET();
    if (code == 200) {
      String rss = http.getString();
      int pos = 0;
      while (true) {
        int s = rss.indexOf("<item>", pos);
        if (s < 0) break;
        int e = rss.indexOf("</item>", s);
        if (e < 0) break;
        String item = rss.substring(s, e+7);
        int ts = item.indexOf("<title>") + 7;
        int te = item.indexOf("</title>", ts);
        String title = (ts>6 && te>ts) ? item.substring(ts,te) : "";
        title.replace("&amp;","&"); title.trim();
        if (isInternational(title)) {
          rssHasIndia = true;
          Serial.printf("[RSS] India: %s\n", title.c_str());
          break;
        }
        pos = e + 7;
      }
    }
    http.end();
  }

  if (!rssHasIndia) {
    Serial.println("[!] No India intl match in RSS.");
    fetching = false;
    return;
  }

  // Step 2: Cricbuzz URL
  String cbUrl = findIndiaCricbuzzUrl();
  if (cbUrl.length() > 0) {
    Serial.printf("[CB] Fetching: %s\n", cbUrl.c_str());
    if (fetchCricbuzzMatch(cbUrl)) matchFound = true;
  }

  if (!matchFound) {
    Serial.println("[!] All sources failed.");
  } else {
    Serial.printf("[OK] Score='%s' CRR='%s' Bat=%d Bowl=%d Recent='%s'\n",
                  matchScore.c_str(), crrStr.c_str(),
                  batterCount, bowlerCount, recentBalls.c_str());
  }
  fetching = false;
}

// =====================================================
//  Display
// =====================================================

void drawLoading(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.drawStr(10, 22, "IND CRICKET");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(22, 36, "Score Monitor");
  u8g2.drawLine(0, 40, 128, 40);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 54, msg);
  u8g2.sendBuffer();
}

void drawNoMatch() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(20, 10, "IND Cricket");
  u8g2.drawLine(0, 13, 128, 13);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 28, "No India intl match");
  u8g2.drawStr(2, 38, "is live right now.");
  u8g2.drawStr(2, 52, "Next check in 30s");
  u8g2.sendBuffer();
}

void drawFetching() {
  char dots[5]; memset(dots, '.', sizeof(dots)-1); dots[4]=0;
  int numDots = dotCount % 4;
  for (int i = numDots; i < 4; i++) dots[i] = ' ';
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(20, 10, "IND Cricket");
  u8g2.drawLine(0, 13, 128, 13);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(22, 36, "Fetching score");
  u8g2.drawStr(58, 48, dots);
  u8g2.sendBuffer();
}

void drawHeader(const char* title) {
  u8g2.setFont(u8g2_font_5x7_tr);
  
  // Show ESP32 CPU (estimated %) and Free RAM (KB)
  uint32_t freeRam = ESP.getFreeHeap() / 1024;
  String sysStr = "C:" + String(cpuPct) + "% R:" + String(freeRam) + "K";
  int sysX = 122 - (int)(sysStr.length() * 5);
  
  u8g2.drawStr(0, 8, title);
  u8g2.drawStr(sysX, 8, sysStr.c_str());
  u8g2.drawLine(0, 10, 128, 10);
  
  // Blinking live dot at the far right
  if ((millis() / 600) % 2 == 0) u8g2.drawDisc(125, 4, 2);
}

// ── Screen 0: Score overview ────────────────────────────
// IND LIVE                     [cricbuzz] *
// ------------------------------------------
// IND 323-7 (48.4)
// CRR:6.64          3rd ODI
// India opt to bat
// ------------------------------------------
// upd 8s ago                          1/3
void drawScoreScreen() {
  u8g2.clearBuffer();
  drawHeader("IND LIVE");

  u8g2.setFont(u8g2_font_6x10_tr);
  String sc = matchScore;
  if ((int)sc.length() > 21) sc = sc.substring(0, 21);
  u8g2.drawStr(0, 24, sc.c_str());

  u8g2.setFont(u8g2_font_5x7_tr);
  if (crrStr.length() > 0) {
    u8g2.drawStr(0, 35, ("CRR:" + crrStr).c_str());
  }
  if (matchDesc.length() > 0) {
    String d = matchDesc;
    if ((int)d.length() > 10) d = d.substring(0, 10);
    u8g2.drawStr(70, 35, d.c_str());
  }
  if (matchStatus.length() > 0) {
    String st = matchStatus;
    if ((int)st.length() > 25) st = st.substring(0, 25);
    u8g2.drawStr(0, 46, st.c_str());
  }

  u8g2.drawLine(0, 53, 128, 53);
  u8g2.setFont(u8g2_font_4x6_tr);
  uint32_t s = (millis() - lastFetch) / 1000;
  char ft[22];
  if (s < 60) sprintf(ft, "upd %us ago", (unsigned)s);
  else        sprintf(ft, "upd %um ago", (unsigned)(s/60));
  u8g2.drawStr(0, 62, ft);
  u8g2.drawStr(108, 62, "1/3");
  u8g2.sendBuffer();
}

// ── Screen 1: Batters ───────────────────────────────────
// BATTING                          [cricbuzz] *
// ------------------------------------------
// Batter     R   B  4s  6s   SR
// ------------------------------------------
// KL Rahul* 103  80   9   3  128
// G.Brar      7   5   1   0  140
//                                       2/3
//
// Column layout (128px, 5x7 font ~6px/char):
//   Name:  0-57  (max 9 chars + optional *)
//   R:    58-69
//   B:    70-81
//   4s:   82-91
//   6s:   92-101
//   SR:  102-127  (3 digits, int)
void drawBattersScreen() {
  u8g2.clearBuffer();
  drawHeader("BATTING");

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0,   19, "Batter");
  u8g2.drawStr(52,  19, " R");
  u8g2.drawStr(69,  19, " B");
  u8g2.drawStr(84,  19, "4s");
  u8g2.drawStr(96,  19, "6s");
  u8g2.drawStr(110, 19, "SR");
  u8g2.drawLine(0, 21, 128, 21);

  u8g2.setFont(u8g2_font_5x7_tr);
  int yPos[] = {33, 46};

  for (int i = 0; i < min(batterCount, 2); i++) {
    int y = yPos[i];
    // 8 chars for name, leave room for * suffix
    String nm = shortName(batters[i].name, 8);
    if (batters[i].onStrike) nm += "*";
    u8g2.drawStr(0, y, nm.c_str());

    char buf[6];
    sprintf(buf, "%3d", batters[i].runs);    u8g2.drawStr(48,  y, buf);
    sprintf(buf, "%3d", batters[i].balls);   u8g2.drawStr(65,  y, buf);
    sprintf(buf, "%2d", batters[i].fours);   u8g2.drawStr(82,  y, buf);
    sprintf(buf, "%2d", batters[i].sixes);   u8g2.drawStr(94,  y, buf);
    sprintf(buf, "%3d", (int)batters[i].sr); u8g2.drawStr(106, y, buf);
  }
  if (batterCount == 0) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(10, 37, "No batter data");
  }

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawLine(0, 53, 128, 53);
  u8g2.drawStr(108, 62, "2/3");
  u8g2.sendBuffer();
}

// ── Screen 2: Bowlers + Recent balls ───────────────────
// BOWLING                          [cricbuzz] *
// ------------------------------------------
// Bowler       O   M   R  W  ECO
// J.Seales*  9.4   0  56  3  5.8
// A.Joseph     9   1  59  0  6.5
// ------------------------------------------
// Recent: 0 4 1 1 0 1              3/3
//
// Column layout:
//   Name:  0-57  (max 8 chars + *)
//   O:    56-68  (e.g. "9.4")
//   M:    68-77
//   R:    77-88
//   W:    88-97
//   ECO:  97-127 (e.g. "5.8")
void drawBowlersScreen() {
  u8g2.clearBuffer();
  drawHeader("BOWLING");

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0,   19, "Bowler");
  u8g2.drawStr(52,  19, "O");
  u8g2.drawStr(69,  19, "M");
  u8g2.drawStr(84,  19, "R");
  u8g2.drawStr(98,  19, "W");
  u8g2.drawStr(111, 19, "ECO");
  u8g2.drawLine(0, 21, 128, 21);

  u8g2.setFont(u8g2_font_5x7_tr);
  int yPos[] = {31, 42};

  for (int i = 0; i < min(bowlerCount, 2); i++) {
    int y = yPos[i];
    String nm = shortName(bowlers[i].name, 8);
    if (bowlers[i].bowling) nm += "*";
    u8g2.drawStr(0, y, nm.c_str());

    String ov = bowlers[i].overs;
    if ((int)ov.length() > 4) ov = ov.substring(0,4);
    u8g2.drawStr(48, y, ov.c_str());

    char buf[6];
    sprintf(buf, "%2d",  bowlers[i].maidens);  u8g2.drawStr(66, y, buf);
    sprintf(buf, "%3d",  bowlers[i].runs);     u8g2.drawStr(78, y, buf);
    sprintf(buf, "%2d",  bowlers[i].wickets);  u8g2.drawStr(95, y, buf);
    
    int eI = (int)bowlers[i].economy;
    int eF = (int)round((bowlers[i].economy - eI) * 10);
    sprintf(buf, "%d.%d", eI, eF);            u8g2.drawStr(107, y, buf);
  }
  if (bowlerCount == 0) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(10, 32, "No bowler data");
  }

  u8g2.drawLine(0, 48, 128, 48);
  u8g2.setFont(u8g2_font_4x6_tr);
  String recLine = "Recent: " + (recentBalls.length() > 0 ? recentBalls : "---");
  // Limit length to leave room for [cb]
  if ((int)recLine.length() > 22) recLine = recLine.substring(0, 22);
  u8g2.drawStr(0, 57, recLine.c_str());
  u8g2.drawStr(113, 57, "[cb]");

  u8g2.drawLine(0, 59, 128, 59);
  u8g2.drawStr(108, 64, "3/3");
  u8g2.sendBuffer();
}

// =====================================================
//  Setup & Loop
// =====================================================
void setup() {
  Serial.begin(115200);
  u8g2.begin();

  drawLoading("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500); Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawLoading("WiFi failed! Reboot.");
    while (true) delay(1000);
  }
  Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());

  drawLoading("Fetching score...");
  fetchScore();
  lastFetch    = millis();
  lastScreen   = millis();
  lastCpuCalc  = millis();
}

void loop() {
  uint32_t loopStart = millis();
  uint32_t now = loopStart;

  if (now - lastFetch >= FETCH_INTERVAL) {
    fetchScore();
    lastFetch = millis();
  }

  if (!fetching && matchFound && (now - lastScreen >= SCREEN_INTERVAL)) {
    currentScreen = (currentScreen + 1) % 3;
    lastScreen = now;
  }

  if (now - lastAnim >= ANIM_INTERVAL) {
    dotCount++;
    lastAnim = now;
  }

  if (fetching) {
    drawFetching();
  } else if (!matchFound) {
    drawNoMatch();
  } else {
    switch (currentScreen) {
      case 0: drawScoreScreen();   break;
      case 1: drawBattersScreen(); break;
      case 2: drawBowlersScreen(); break;
    }
  }
  
  uint32_t loopEnd = millis();
  busyLoopTime += (loopEnd - loopStart);

  delay(80);
  
  totalLoopTime += (millis() - loopStart);
  
  // Calculate CPU % every 2 seconds
  if (millis() - lastCpuCalc >= 2000) {
    if (totalLoopTime > 0) {
      cpuPct = (busyLoopTime * 100) / totalLoopTime;
    }
    busyLoopTime = 0;
    totalLoopTime = 0;
    lastCpuCalc = millis();
  }
}