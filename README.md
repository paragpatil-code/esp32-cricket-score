# ESP32 India Cricket Score Monitor

A feature-rich, standalone ESP32 project that fetches and displays live cricket scores for Indian international matches on an SH1106 128x64 OLED display.

It automatically discovers live India international matches, fetches detailed live scorecards, and cycles through various screens showing the overall score, current batters, and current bowlers in real-time.

## Features

- **Automatic Match Discovery**: Uses Cricinfo RSS to discover if an India international match is currently live.
- **Detailed Live Data**: Scrapes detailed scorecard data from Cricbuzz (Score, CRR, Required Run Rate, Match Status, Toss info).
- **Auto-Rotating Screens**: Cycles through three detailed views every 10 seconds:

  **Screen 1: Overview**
  Overall match score, Current Run Rate (CRR), match description/RRR, and live status.
  ```text
  IND LIVE             C:12% R:134K *
  -----------------------------------
  IND 323-7 (48.4)
  CRR:6.64          3rd ODI
  India opt to bat
  -----------------------------------
  upd 8s ago                    1/3
  ```

  **Screen 2: Batting**
  In-depth stats for the two current batters (Runs, Balls, Fours, Sixes, Strike Rate).
  ```text
  BATTING              C:15% R:134K *
  -----------------------------------
  Batter     R   B  4s  6s   SR
  -----------------------------------
  KL Rahul* 103  80   9   3  128
  G.Brar      7   5   1   0  140
  -----------------------------------
                                2/3
  ```

  **Screen 3: Bowling**
  In-depth stats for the current bowlers (Overs, Maidens, Runs, Wickets, Economy) and recent balls.
  ```text
  BOWLING              C:14% R:134K *
  -----------------------------------
  Bowler       O   M   R  W  ECO
  -----------------------------------
  J.Seales*  9.4   0  56  3  5.8
  A.Joseph     9   1  59  0  6.5
  -----------------------------------
  Recent: 0 4 1 1 0 1   [cb]    3/3
  ```

- **System Stats Monitor**: Displays estimated CPU usage and Free RAM (in KB) in the header.
- **Real-time Status**: Displays "time since last update", live blinking dots, and WiFi connection status.

## Hardware Requirements

- **ESP32 Development Board**: Any standard ESP32 board with WiFi capabilities.
- **OLED Display**: SH1106 128x64 OLED display (I2C interface).
  - If using a different OLED (like SSD1306), you will need to change the U8g2 initialization line in the code.
- **Wires**: For connecting the display to the ESP32 (VCC, GND, SCL, SDA).

## Software Dependencies

This project relies on the following Arduino libraries. You can install them via the Library Manager in the Arduino IDE:

- **U8g2** by olikraus (Handles drawing to the OLED display)
- *Built-in ESP32 Libraries*: `WiFi.h`, `HTTPClient.h`, `Wire.h`

## Installation and Setup

1. **Clone or Download** this repository/file to your local machine.
2. **Open** `cricket_score.cpp` (or rename it to `.ino` if using the standard Arduino IDE) in your preferred development environment (Arduino IDE, PlatformIO, etc.).
3. **Configure WiFi**:
   Locate the User Config section at the top of the file and enter your WiFi credentials:
   ```cpp
   // ==================== USER CONFIG ====================
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   // =====================================================
   ```
4. **Select Board**: Choose your specific ESP32 board from the Tools menu.
5. **Compile and Upload** the code to your ESP32.

## How it Works (Data Pipeline)

Since there are no free, high-quality, undocumented Cricket APIs available reliably, this project uses a three-step web scraping cascade:

1. **Step 1 (Discovery)**: Fetches the lightweight Cricinfo RSS feed to see if any of the currently live international matches involve "India".
2. **Step 2 (Routing)**: If an India match is found, it fetches the Cricbuzz live-scores HTML page and parses it to locate the exact URL for the live India match.
3. **Step 3 (Scraping)**: Connects to the specific match page on Cricbuzz and uses custom HTML parsing to extract the OpenGraph title, current run rate, player stats (from grid classes), and recent balls without needing heavy JSON parsing or API keys.

## Customization

- **Change Update Intervals**:
  You can modify how often the ESP32 fetches new data or changes screens by tweaking these constants:
  ```cpp
  const uint32_t FETCH_INTERVAL  = 30000; // Fetch data every 30 seconds
  const uint32_t SCREEN_INTERVAL = 10000; // Rotate screens every 10 seconds
  ```
- **Change the Display Type**:
  If you are using an SSD1306 display instead of an SH1106, change the U8g2 constructor:
  ```cpp
  // Old (SH1106)
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

  // New (SSD1306 example)
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
  ```

## Troubleshooting

- **Display is Blank**: Double-check your I2C wiring (SDA and SCL pins). Ensure the correct U8g2 constructor is being used for your specific display model.
- **WiFi Failed! Reboot**: Ensure your `WIFI_SSID` and `WIFI_PASSWORD` are correct and that the ESP32 is within range of your router (2.4GHz networks only).
- **"No India intl match is live right now"**: The device successfully connected and fetched data, but there is no live Indian international match currently playing.

## Note to Cricbuzz and CricInfo

This is a non-commercial, personal hobby project for monitoring live cricket scores on a small microcontroller. If anyone from the Cricbuzz team or CricInfo team has concerns, wishes to request changes, or would like this project removed, please contact me at: contact@paragpatil.in.

## License

This project is open-source. Feel free to modify and use it for your personal monitoring!
