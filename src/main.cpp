#include <Arduino.h>
#include "epd_driver.h"
#include <FS.h>
#include <SD.h>
#include <time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <utilities.h>
#include "FontFiles/opensans6.h"
#include "FontFiles/opensans8.h"
#include "FontFiles/opensans10.h"
#include "FontFiles/opensans26.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Credentials.h"
#include <Preferences.h>

const char *VERSION = "Version: b1 241116"; // Ctrl+Shift+I --> Date (Extension: Insert Date String)
const char *ntpServer1 = "de.pool.ntp.org";
const char *timeZone = "CET-1CEST,M3.5.0/03,M10.5.0/03"; // TimeZone rule for Europe/Rome including daylight adjustment rules (optional)
uint8_t *frameBuffer = NULL;
JsonDocument doc;                                               // Memory for the JSON data before deserializing
char buffer[200];                                               // Buffer for sprintf
int price[2][24];                                               // 24h = 24 prices, each for the current and the next day
int minPrice[2] = {0, 0}, maxPrice[2] = {0, 0};                 // Each for the current and the next day
long priceAverage[2] = {0, 0};                                  // Average value, each for the current and the next day
long average[2] = {0, 0};                                       // Here the average from 13-24h of the current day and 0-23h of the next day for ePaper display
int minVal = 30000, maxVal = 0, minRounded, maxRounded, spread; // For ePaper display
GFXfont currentFont;                                            // Variable for the current font and size
uint16_t graphHeight = 440;                                     // Height of the chart
double deepSleepTime;                                           // Variable for the duration of the next deep sleep
bool updateReady = false;                                       // Variable to update the EPD only if new prcies are available based on the time
bool wifiOK = false;
bool deepSleepOK = false;
bool deepSleepActive = true; // To disable DeepSleep for easier uploading while working on the code
bool tibberPriceOK = false;
bool tibberPriceUpdated = false; // Check if the prices for the next day are available
bool timeOK = false;
bool debugging = true;
Preferences preferences; // Create Preferences instance
u32_t counterBrownOut = 0;
time_t timeNow; // global variable for current time as Epoch
time_t timeNow2;
time_t nextUpdateEpoch;
struct tm tmNow;        // global structure for current time as readable time
struct tm tmNextUpdate; // global structure for next update time as readable time - I used a global structure instead of on in the sub routine for debugging reasons

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
}

void timeAvailable(struct timeval *t)
{
    Serial.println("[WiFi]: Got time adjustment from NTP!");
    // rtc.hwClockWrite();
}

void setFont(GFXfont const &font)
{
    currentFont = font;
}

void extractPricesFromJson() // Extract prices from JSON string and output in a compact table
{
    char buffer[200];
    Serial.println("extractPricesFromJson:");
    char day[2][9] = {"today", "tomorrow"};
    double priceInDouble;
    priceAverage[0] = 0;
    priceAverage[1] = 0;
    for (int dayIndex = 0; dayIndex < 2; dayIndex++)
    {
        for (int hour = 0; hour < 24; hour++)
        {
            priceInDouble = doc["data"]["viewer"]["homes"][0]["currentSubscription"]["priceInfo"][day[dayIndex]][hour]["total"];
            price[dayIndex][hour] = int(10000 * priceInDouble);
            sprintf(buffer, "Day= %9s, dayIndex=%2d, hour=%2d, Price= %4d\n", day[dayIndex], dayIndex, hour, price[dayIndex][hour]);
            Serial.print(buffer);
            int hourAdjusted;
            if (hour == 23)
                hourAdjusted = 0;
            else
                hourAdjusted = hour;
            if (hour > 0)
            {
                if (price[dayIndex][minPrice[dayIndex]] > price[dayIndex][hourAdjusted])
                    minPrice[dayIndex] = hourAdjusted;
                if (price[dayIndex][maxPrice[dayIndex]] < price[dayIndex][hourAdjusted])
                    maxPrice[dayIndex] = hourAdjusted;
            }
            priceAverage[dayIndex] = price[dayIndex][hour] + priceAverage[dayIndex]; // Calculate average value part 1
        }
        priceAverage[dayIndex] = priceAverage[dayIndex] / 24; // Calculate average value part 2
    }
    Serial.println("\n            Today        ||||             Tomorrow"); // Output in compact table
    for (int hourAdjusted = 0; hourAdjusted < 12; hourAdjusted++)
    {
        sprintf(buffer, "hour=%2d: %4d || hour=%2d: %4d |||| hour=%2d: %4d || hour=%2d: %4d ||\n", hourAdjusted, price[0][hourAdjusted], hourAdjusted + 12, price[0][hourAdjusted + 12], hourAdjusted, price[1][hourAdjusted], hourAdjusted + 12, price[1][hourAdjusted + 12]);
        Serial.print(buffer);
    }
    sprintf(buffer, " Min hour: %2d, %4d,        |||| Min hour: %2d, %4d, \n", minPrice[0], price[0][minPrice[0]], minPrice[1], price[1][minPrice[1]]);
    sprintf(buffer + strlen(buffer), " Max hour: %2d, %4d,        |||| Max hour: %2d, %4d, \n", maxPrice[0], price[0][maxPrice[0]], maxPrice[1], price[1][maxPrice[1]]);
    sprintf(buffer + strlen(buffer), "Average: %4d         |||| Average: %4d\n\n", priceAverage[0], priceAverage[1]);
    Serial.print(buffer);
    sprintf(buffer, "   <hour:%2d, >hour:%2d, =:%4d  ||||   <hour:%2d, >hour:%2d, =:%4d \n", minPrice[0], maxPrice[0], priceAverage[0], minPrice[1], maxPrice[1], priceAverage[1]);
    Serial.print(buffer);
}

void calculateEpaperMinMax()
{
    average[0] = 0; // Here the average from 13-24h of the current day and 0-23h of the next day for ePaper display
    average[1] = 0;
    minVal = 30000, maxVal = 0, minRounded, maxRounded, spread; // For ePaper display
    int sumTomorrow = 0;

    for (int hour = 13; hour < 24; hour++)
    { // Determine min, max, and average values for the first day, 13-23h
        if (price[0][hour] < minVal)
            minVal = price[0][hour];
        if (price[0][hour] > maxVal)
            maxVal = price[0][hour];
        average[0] = average[0] + price[0][hour];
    }

    for (int hour = 0; hour < 24; hour++)
    {
        sumTomorrow = sumTomorrow + price[1][hour];
    }
    if (sumTomorrow != 0)
    {
        for (int hour = 0; hour < 24; hour++)
        { // Determine min, max, and average values for the second day, 0-23h
            if (price[1][hour] < minVal)
                minVal = price[1][hour];
            if (price[1][hour] > maxVal)
                maxVal = price[1][hour];
            average[1] = average[1] + price[1][hour];
        }
        tibberPriceUpdated = true;
    }

    average[0] = average[0] / 11;          // Day1 from 13-23h
    average[1] = average[1] / 24;          // Day2 from 0-23h
    minRounded = (minVal / 100 - 2) * 100; // Rounded down to nearest 100 -200
    maxRounded = (maxVal / 100 + 1) * 100; // Rounded up to nearest 100 +100
    spread = maxRounded - minRounded;      // Spread
    sprintf(buffer, "Average0: %4d, Average1: %4d, Minimum: %4d, Maximum: %4d, minRounded: %4d, maxRounded: %4d, spread: %4d\n",
            average[0], average[1], minVal, maxVal, minRounded, maxRounded, spread);
    Serial.print(buffer);
}

void fetchTibberPrices()
{
    WiFiClientSecure client; // HTTPS !!!
    client.setInsecure();    // the magic line, use with caution
    HTTPClient https;
    strcpy(buffer, "Bearer ");
    strcat(buffer, token);
    https.begin(client, tibberApi);
    https.addHeader("Content-Type", "application/json"); // Add necessary headers
    https.addHeader("Authorization", buffer);            // Add necessary headers
    String payload = "{\"query\": \"{viewer { homes { currentSubscription{ priceInfo{ today{ total  } tomorrow { total  }}}}}}\"} ";
    Serial.print("STRING PAYLOAD: ");
    Serial.println(payload);
    int httpCode = https.POST(payload);
    if (httpCode == HTTP_CODE_OK)
    {
        String response = https.getString();
        Serial.println(response);
        DeserializationError error = deserializeJson(doc, response);
        if (error)
            Serial.println("\n\n################################# Error parsing JSON response ###########################\n");
        extractPricesFromJson();
        tibberPriceOK = true; // Error handling
    }
    else
    {
        Serial.println("Something went wrong");
        Serial.println(httpCode);
    }
    https.end();
    client.stop(); // Disconnect from the server
}

void calculateDeepSleepTime()
{
    // struct tm timeinfo;
    if (timeOK)
    {

        // Calculate the time until the next update of energy prices (13:15 Uhr) with an intermediate step at 12:00 to avoid problems based on inaccurate wakeup times
        // struct tm nextUpdate = tmNow;
        // struct tm tmNextUpdate;
        // localtime_r(&timeNow, &tmNextUpdate);

        tmNextUpdate = tmNow;

        tmNextUpdate.tm_hour = 12;
        tmNextUpdate.tm_min = 0;
        tmNextUpdate.tm_sec = 0;
        if (tmNow.tm_hour == 12 || (tmNow.tm_hour == 13 && tmNow.tm_min < 15))
        {
            tmNextUpdate.tm_hour = 13;
            tmNextUpdate.tm_min = 15;
            tmNextUpdate.tm_sec = 0;
        }

        // If the prices were already updated today, add one day
        if (tmNow.tm_hour > 13 || (tmNow.tm_hour == 13 && tmNow.tm_min >= 15))
        {
            tmNextUpdate.tm_mday += 1;
            updateReady = true; // updateReady will be true if the current time is after 13:15
        }

        // Time in seconds until the next update
        // time_t nextUpdateEpoch = mktime(&tmNextUpdate);
        // deepSleepTime = difftime(nextUpdateEpoch, timeNow);
        // deepSleepOK = true; // Error handling

        // time_t nextUpdateEpoch = mktime(&tmNextUpdate);
        nextUpdateEpoch = mktime(&tmNextUpdate);
        if (nextUpdateEpoch == -1) // In case of an error during the time calculation by mktime -1 is the return value.
        {
            Serial.println("Error: mktime() could not calculate the time correctly.");
            deepSleepTime = 3600; // Fallback
            deepSleepOK = false;
        }
        else
        {
            if (timeNow < 1731711600) // 1731711600 Unix epox time of today (16.11.2024)
            {
                timeNow = time(NULL);
            }
            deepSleepTime = difftime(nextUpdateEpoch, timeNow);
            deepSleepOK = true;
        }
    }
    else
    {
        deepSleepTime = 3600;
    }
}

void epaperOutput()
{
    epd_init();
    epd_poweron();
    epd_clear();
    setFont(OpenSans8);
    int cursor_x;
    int cursor_y;

    // ####### Draw bars ##################################################
    int d = 0, p = 0;
    for (int h = 13; h < 48; h++)
    {
        if (h > 23 && d == 0)
        {
            d = 1;
            p = 24;
        }
        int bar_height = long(graphHeight * ((price[d][h - p] - minRounded) * 100 / spread) / 100); // max. graphHeight pixels high
        int hh = (h - 13) * 25;                                                                     // Last number affects the y-filling of the bars --> must also be adjusted in the labeling
        epd_fill_rect(/*x0*/ 70 + hh, /*y=*/EPD_HEIGHT - 80 - bar_height, 8, bar_height, 0x0000, frameBuffer);
    }
    // ####### End of bar drawing #########################################

    // ####### Draw Y-axis ################################################
    int subdivision = 6;
    int delta_spread = spread / subdivision / 100; // rounded to whole cents
    int y_axis = 0;                                // counts the horizontal auxiliary lines of the y-axis
    while (true)
    {
        int y_distance = EPD_HEIGHT - (80 + (y_axis * graphHeight * delta_spread / (spread / 100)));
        epd_draw_line(/*x=*/60, y_distance, EPD_WIDTH - 20, y_distance, 0x0000, frameBuffer);
        cursor_x = 15;
        cursor_y = y_distance;
        setFont(OpenSans10);
        writeln((GFXfont *)&currentFont, (char *)String(delta_spread * y_axis + minRounded / 100).c_str(), &cursor_x, &cursor_y, NULL);

        if (y_distance < (EPD_HEIGHT - (graphHeight + 0)))
            break;
        y_axis++;
        if (y_axis > 10)
            break; // emergency stop
    }

    cursor_x = 13;
    cursor_y = EPD_HEIGHT - 515; // was 465 at first, later 505
    writeln((GFXfont *)&currentFont, "Cent/kWh", &cursor_x, &cursor_y, NULL);
    cursor_y = EPD_HEIGHT - 451;
    // ####### End of Y-axis writing ######################################

    // ####### Write X-axis ################################################
    int yyy = EPD_HEIGHT - 50;
    for (int yt = 0; yt < 18; yt++)
    {                          // Label X-axis
        int ytt = yt * 2 * 25; // Same number as above must be used :)
        int h = 13 + 2 * yt;
        if (h > 23)
            h = h - 24;
        int sp = 0;
        if (h < 10)
            sp = 6;
        cursor_x = 62 + ytt + sp;
        cursor_y = yyy;
        writeln((GFXfont *)&currentFont, (char *)String(h).c_str(), &cursor_x, &cursor_y, NULL);
    }

    struct tm tmDay;
    time_t timeDay = time(NULL);
    // time(&now);
    localtime_r(&timeDay, &tmDay);
    strftime(buffer, 128, "%a, %d.%m.                                     Stunde                                     ", &tmDay);
    timeDay = (timeDay + 86400);
    localtime_r(&timeDay, &tmDay);

    strftime(buffer + strlen(buffer), 128, "%a, %d.%m", &tmDay);
    cursor_x = 135;
    cursor_y = EPD_HEIGHT - 16;
    writeln((GFXfont *)&currentFont, buffer, &cursor_x, &cursor_y, NULL);
    // ####### End of X-axis writing ######################################

    // ####### Mean value lines ###########################################
    // int mw0 = EPD_HEIGHT - 80 - (440 * (mean_value[0] - min_rounded)) / spread; // Mean value from 13-23h
    int mw0 = EPD_HEIGHT - 80 - (graphHeight * (priceAverage[0] - minRounded)) / spread; // Mean value from 0 - 23h
    int mw1 = EPD_HEIGHT - 80 - (graphHeight * (average[1] - minRounded)) / spread;
    sprintf(buffer, "Mean value lines: mw0: %4d, mw1: %4d, Mean price 0: %4d, Mean value 1: %4d, \n", mw0, mw1, priceAverage[0], average[1]);
    Serial.print(buffer);

    for (p = 0; p < 11 * 25; p += 8)
        epd_fill_rect(/*x0*/ 60 + p, /*y=*/mw0, 2, 5, 0x0080, frameBuffer);
    for (p = 0; p < 24 * 25; p += 8)
        epd_fill_rect(/*x0*/ 60 + 11 * 25 + 5 + p, /*y=*/mw1, 2, 5, 0x0080, frameBuffer);
    // ####### End of mean value lines ####################################

    // ####### Error handling #############################################
    // if (wifiOK == false)
    // {
    //     cursor_x = 260;
    //     cursor_y = 50;
    //     setFont(OpenSans26);
    //     writeln((GFXfont *)&currentFont, "No Wifi", &cursor_x, &cursor_y, NULL);
    // }

    if (deepSleepOK == false || deepSleepActive == false)
    {
        cursor_x = 260;
        cursor_y = 100;
        setFont(OpenSans26);
        writeln((GFXfont *)&currentFont, "No Deepsleep", &cursor_x, &cursor_y, NULL);
    }

    // if (tibberPriceOK == false)
    // {
    //     cursor_x = 260;
    //     cursor_y = 150;
    //     setFont(OpenSans26);
    //     writeln((GFXfont *)&currentFont, "No Price", &cursor_x, &cursor_y, NULL);
    // }
    // ####### End of error handling ######################################

    // ####### Debugging #############################################
    if (debugging == true)
    {
        double hoursToNextUpdate = deepSleepTime / 3600;
        cursor_x = 260;
        cursor_y = EPD_HEIGHT - 515;
        setFont(OpenSans10);
        writeln((GFXfont *)&currentFont, (char *)String(esp_reset_reason()).c_str(), &cursor_x, &cursor_y, NULL);
        cursor_x = 300;
        writeln((GFXfont *)&currentFont, (char *)String(hoursToNextUpdate).c_str(), &cursor_x, &cursor_y, NULL);
        cursor_x = 400;
        writeln((GFXfont *)&currentFont, (char *)String(counterBrownOut).c_str(), &cursor_x, &cursor_y, NULL);
        cursor_x = 450;
        strftime(buffer, 128, "NextUpdate: %d.%m.%y %H:%M ", &tmNextUpdate);
        writeln((GFXfont *)&currentFont, buffer, &cursor_x, &cursor_y, NULL);
        cursor_x = 260;
        cursor_y = EPD_HEIGHT - 495;
        setFont(OpenSans8);
        writeln((GFXfont *)&currentFont, (char *)String(timeNow).c_str(), &cursor_x, &cursor_y, NULL);
        writeln((GFXfont *)&currentFont, (char *)" timeNow ", &cursor_x, &cursor_y, NULL);
        writeln((GFXfont *)&currentFont, (char *)String(timeNow2).c_str(), &cursor_x, &cursor_y, NULL);
        writeln((GFXfont *)&currentFont, (char *)" timeNow2", &cursor_x, &cursor_y, NULL);
        cursor_x = 260;
        cursor_y = EPD_HEIGHT - 480;
        writeln((GFXfont *)&currentFont, (char *)String(nextUpdateEpoch).c_str(), &cursor_x, &cursor_y, NULL);
        writeln((GFXfont *)&currentFont, (char *)" nextUpdateEpoch", &cursor_x, &cursor_y, NULL);
    }

    // ####### End of Debugging ######################################

    // ####### Last update and version ####################################
    // time(&now);             // Query time again to ensure the correct date
    // localtime_r(&now, &tm); // Transfer time to tm with the correct time zone
    cursor_x = 5;
    cursor_y = EPD_HEIGHT - 0;
    setFont(OpenSans6);
    strftime(buffer, 128, "Update: %d.%m.%y %H:%M ", &tmNow);
    sprintf(buffer + strlen(buffer), VERSION);
    writeln((GFXfont *)&currentFont, buffer, &cursor_x, &cursor_y, NULL);

    // ####### End of last update and version #############################

    epd_draw_grayscale_image(epd_full_screen(), frameBuffer);
    epd_poweroff_all();
    Serial.println("ePaper writing finished");
}

void epaperErrorOutput()
{
    int cursor_x;
    int cursor_y;
    epd_init();
    epd_poweron();
    epd_clear();

    if (wifiOK == false)
    {
        cursor_x = 260;
        cursor_y = 50;
        setFont(OpenSans26);
        writeln((GFXfont *)&currentFont, "No Wifi", &cursor_x, &cursor_y, NULL);
    }

    if (timeOK == false)
    {
        cursor_x = 260;
        cursor_y = 100;
        setFont(OpenSans26);
        writeln((GFXfont *)&currentFont, "No Time", &cursor_x, &cursor_y, NULL);
    }

    if (tibberPriceOK == false)
    {
        cursor_x = 260;
        cursor_y = 150;
        setFont(OpenSans26);
        writeln((GFXfont *)&currentFont, "No Price", &cursor_x, &cursor_y, NULL);
    }
    epd_draw_grayscale_image(epd_full_screen(), frameBuffer);
    epd_poweroff_all();
}

void debug()
{
    preferences.begin("Storage", false); // Open the Preferences Storage
    counterBrownOut = preferences.getUInt("counterBrownOut", 0);
    if (esp_reset_reason() == 9)
    {
        counterBrownOut++;
        preferences.putUInt("counterBrownOut", counterBrownOut);
    }
    preferences.end();
}

void setup()
{
    Serial.begin(115200);

    // ############### Connecting to Wifi ######################################################
    // Set WiFi to station mode and disconnect from an AP if it was previously connected
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Connecting to WiFi..."); // Wait for WiFi connection
    for (int timeout = 120; timeout >= 0; timeout--)
    { // wait max. 60 seconds for the Wifi connection
        if (WiFi.status() == WL_CONNECTED)
        {
            wifiOK = true;
            break;
        }
        delay(500);
        Serial.print(".");
    }
    WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    // ############### End of Connecting to Wifi ########################################################

    // ############### Configuration of the time server and get current time ######################################################
    // set notification call-back function
    sntp_set_time_sync_notification_cb(timeAvailable);
    configTzTime(timeZone, ntpServer1);
    timeNow = time(NULL); // Current time (Epoch) as time_t (seconds since 01.01.1970)
    timeNow2 = time(NULL);
    localtime_r(&timeNow, &tmNow); // Transfer time to tm with the correct time zone
    if (getLocalTime(&tmNow))
    {
        timeOK = true;
    }

    // ############### End of Configuration of the time server and get current time ########################################################

    // ############### Framebuffer Memory Allocation and Initialization ######################################################
    frameBuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!frameBuffer)
    {
        Serial.println("alloc memory failed !!!");
        while (1)
            ;
    }
    memset(frameBuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    // ############### End of Framebuffer Memory Allocation and Initialization ########################################################

    // ############### Subroutine calls ######################################################

    // calculateDeepSleepTime();
    // if (debugging == true)
    // {
    //     debug();
    // }

    if (wifiOK == true && timeOK == true)
    {
        calculateDeepSleepTime();

        if (debugging == true)
        {
            debug();
        }

        if (updateReady == true || esp_reset_reason() == 0 || esp_reset_reason() == 1)
        {
            fetchTibberPrices();
            calculateEpaperMinMax();
            epaperOutput();

            if (tibberPriceUpdated == false && updateReady == true) // sometimes the new prices are delayed, if so this checks for new prices every five minutes
            {
                deepSleepTime = 600;
            }
        }
    }

    if (wifiOK == false || timeOK == false || tibberPriceOK == false)
    {
        epaperErrorOutput();
    }

    // if (updateReady = true)
    // {
    // epd_init();
    // epd_poweron();
    // epd_clear();
    // fetchTibberPrices();
    // calculateEpaperMinMax();
    // epaperOutput();
    // epd_poweroff_all();
    // }

    // if (tibberPriceOK == false) // Try to reload prices, if they are not availablae at 13:15 after 10 minutes
    // {
    //     deepSleepTime = 600;
    // }

    // ############### End of subroutine calls ########################################################

    // ############### Deepsleep ######################################################
    if (deepSleepActive)
    {
        esp_sleep_enable_timer_wakeup(deepSleepTime * 1000000);
        esp_deep_sleep_start();
    }

    // ############### End of Deepsleep ########################################################
}

void loop()
{
    delay(2);
}