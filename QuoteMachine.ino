/*
 * Digital Keychain
 * Inspirational Quotes Retrieval Machine
 * Larry Jing - December 2020
 * 
 * Retrieves inspirational quotes from a web api. Displays time. Makes Magic-8-ball decisions.
 * If new quote not retrieved, backup quotes selected from SD card. 
 * Boots settings and logins from card.
 * 
 * Settings Array:
 * [0] WiFi Delay - Time allotted to each WiFi login (milliseconds)
 * [1] Operation Mode - "1" for Standard, "2" for SD only, "3" for WiFi only, "4" for EEPROM only
 * [2] EEPROM Update - "1" to store last displayed quote in EEPROM, "0" to potentially lose it forever
 * [3] Inactivity Sleep Time - seconds of inactivity for device to enter sleep mode.
 * [4] SD Quote File (min) - The lower bound filename number you want to randomly select from the quotes folder
 * [5] SD Quote File (max) - The upper bound filename number you want to randomly select from the quotes folder
 * [6] Button 1 Scroll Speed - ms between individual display frames (less is faster)
 * [7] Button 2 Scroll Speed - ms between individual display frames (less is faster)
 * [8] Greenwich Mean Time Offset - # hours (Pacific Standard Time is 8 hours behind GMT-8)
 * [9] Daylight Savings Time Offset - # hours (Either 1 or 0)
 * [10] Long Press Register Time - ms for a button press to be considered long press. Three times this forces sleep mode.
 */

#include <Arduino.h>
#include "SSD1306Wire.h"
#include "images.h"
#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "FS.h"
#include "SD_MMC.h"

//SDA: 16   SCL: 0
SSD1306Wire display(0x3c, 16, 0);
WiFiMulti wifiMulti;

#define BUTTON_PIN_BITMASK 0x3000
#define LED_BUILTIN 4
const int BUTTON1 = 13;
const int BUTTON2 = 12;

bool last = true;
bool wifiStatus = false;
bool sleepCom = false;
int one = 0;
int two = 0;
int p_day = 0;
int p_hour = 0;
long lastContact = millis();

// Extra slots allotted for future settings, default values assume no SD card
int settings[11] = {0, 3, 0, 30, 0, 0, 7, 7, -8, 1, 1000};
const char* ntpServer = "pool.ntp.org";
const String weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const String months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const String responses[20] = 
    {"As I see it, yes.",
     "Ask again later.",
     "Better not tell you now.",
     "Cannot predict now.",
     "Concentrate and ask again.",
     "Don’t count on it.",
     "It is certain.",
     "It is decidedly so.",
     "Most likely.",
     "My reply is no.",
     "My sources say no.",
     "Outlook not so good.",
     "Outlook good.",
     "Reply hazy, try again.",
     "Signs point to yes.",
     "Very doubtful.",
     "Without a doubt.",
     "Yes.",
     "Yes, definitely.",
     "You may rely on it."};

// Entire code should run off of the setup
void setup() {
    Serial.begin(115200);
    Serial.println();
    pinMode(2, INPUT_PULLUP);
    pinMode(BUTTON1, INPUT);
    pinMode(BUTTON2, INPUT);
    EEPROM.begin(512);
    display.init();
    display.clear();
    randomSeed(esp_random());
    if (SD_MMC.begin("/sdcard",true) && (SD_MMC.cardType() != CARD_NONE)) {
        Serial.println("SD card located. Booting.");
        boot(SD_MMC);
    } else {
        Serial.println("Card mount failed... Default settings.");
        if (tryWifi(readStringFromEEPROM(400))) {
            Serial.println("EEPROM WiFi connected!");
            settings[1] = 3;
        } else {
            settings[1] = 4;
        }
    } 
    displaySetup();
    pinMode (LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    while (!sleepCom) {
        pinMode(BUTTON1, INPUT);
        pinMode(BUTTON2, INPUT);
        one = digitalRead(BUTTON1);
        two = digitalRead(BUTTON2);
        if ((one + two) == 0) {
            displaySetup();
            if (millis() - lastContact > (settings[3] * 1000)) {
                Serial.println("Inactivity Timeout");
                break;
            }
        } else {
            buttonHandler(one, two);
            lastContact = millis();
        }
        delay(150);
    }
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){displayText("Sleeping...", "");delay(50);}
    displayText("Sleeping...", "");
    delay(1000);
    display.clear();
    display.display();
    pinMode (LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK,ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.println("Going to sleep now");
    esp_deep_sleep_start();
}

// Boots settings from SD card
void boot(fs::FS &fs) {
    File file = fs.open("/resources/settings.txt");
    if (!file) {
        Serial.println("Failed to open file - using default settings.");
        return;
    }
    bool comment = true;
    String line = "";
    int index = 0;
    while (file.available()) {
        char cur = char(file.read());
        if ((!comment && cur == char(10)) && line.length() >= 1) {
            settings[index] = line.toInt();
            index++;
            line = "";
            comment = true; 
        } else if (cur == '#') {
            comment = true;
        } else if (comment && cur == char(10)) {
            comment = false;
        }
        if (!comment && (cur > 32 && cur < 126)) {
            line += cur;
        }
    }
    if (line.length() >= 1) {
        settings[index] = line.toInt();
    }
}

// Logs into WiFi from SD card credentials
void bootWiFi(fs::FS &fs) {
    File file = fs.open("/resources/logins.txt");
    if (file) {
        bool comment = true;
        String line = "";
        while (file.available()) {
            char cur = char(file.read());
            if ((!comment && cur == char(10)) && line.length() >= 1) {
                if (tryWifi(line)) {
                    writeStringToEEPROM(400, line);
                    Serial.println("-------------------------------------");
                    Serial.println("Connected Successfully!");
                    Serial.println("-------------------------------------");
                    return;
                }
                line = "";
                comment = true; 
            } else if (cur == '#') {
                comment = true;
            } else if (comment && cur == char(10)) {
                comment = false;
            }
            if (!comment && (cur >= 32 && cur < 127)) {
                line += cur;
            }
        }
        if (line.length() >= 1 && tryWifi(line)) {
            return;
        }
    }
    Serial.println("Error: changing operation mode to offline (2).");
    settings[1] = 2;      
}

// Attempts individual WiFi login
bool tryWifi(String login) {
    String username = login.substring(0, login.indexOf("@"));
    String password = login.substring(login.indexOf("@") + 1);
    display.clear();
    display.drawXbm(0, 1, 30, 60, wifi);
    display.setFont(Open_Sans_Condensed_Bold_20);
    display.drawString(40, 5, "TRYING:");
    display.setFont(Open_Sans_Condensed_Bold_24);
    display.drawString(28, 30, username);
    display.display();
    Serial.println("-------TRYING:------");
    Serial.println("USERNAME: " + username);
    Serial.println("PASSWORD: " + password);
    Serial.println("--------------------");
    wifiMulti.addAP(username.c_str(), password.c_str());
    int a = millis();
    while (wifiMulti.run() != WL_CONNECTED) {
        if (millis() - a > settings[0]) {
            Serial.println("Unable to login in time.");
            wifiStatus = false;
            return false;
        }
    }
    configTime(settings[8]*3600, settings[9]*3600, ntpServer);
    wifiStatus = true;
    return true;
}

// Main loop method, repeatedly reads button input and directs state (OFFLINE)
void loop() {
    Serial.println("If you're reading this, something's probably wrong.");
    delay(1000);
}

// Takes button inputs and directs state accordingly - assumes at least one button pressed
void buttonHandler(int one, int two) {
    sleepCom = false;
    int preTime = millis();
    if ((one + two) == 2) {
        Serial.print("BOTH ");
        displayText("V       V", "");
        while (digitalRead(BUTTON1) + digitalRead(BUTTON2) == 2) {
            if (millis() - preTime > settings[10]){
                Serial.println("(LONG PRESS)");
                displayText("Credits!", "");
                while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1) {
                    delay(100);
                    if (millis() - preTime > (settings[10] * 3)) {
                        sleepCom = true;
                        return;
                    } 
                }
                scrollText(settings[6], "On-the-go Inspiration. Made by Larry Jing - 2020.");
                break;
            }
        }
        if (millis() - preTime < settings[10]){
            Serial.println("(SHORT PRESS)");
            while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);} 
            magic8();
        }
    } else if (one == 1) {
        Serial.print("Button 1 ");
        displayText("V        ", "");
        while (digitalRead(BUTTON1) == 1) {
            if (millis() - preTime > settings[10]){
                Serial.println("(LONG PRESS)");
                displayText("Current Time", "");
                while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){
                    delay(100);
                    if (millis() - preTime > (settings[10] * 3)) {
                        sleepCom = true;
                        return;
                    }
                } 
                button1b();
                break;
            }
        }
        if (millis() - preTime < settings[10]){
            Serial.println("(SHORT PRESS)");
            while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);} 
            button1();
        }
    } else {
        Serial.println("Button 2 ");
        displayText("        V", "");
        while (digitalRead(BUTTON2) == 1) {
            if (millis() - preTime > settings[10]) {
                Serial.println("(LONG PRESS)");
                displayText("Today's Date", "");
                while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1) {
                    delay(100);
                    if (millis() - preTime > (settings[10] * 3)) {
                        sleepCom = true;
                        return;
                    } 
                }
                button2b();
                break;
            }
        }
        if (millis() - preTime < settings[10]){
            Serial.println("(SHORT PRESS)");
            while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);} 
            button2();
        }
    }
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(50);}
    last = true;
}

// Sets up display in ready state
void displaySetup() {
    if (last) {
        Serial.println("Setting Up Display.");
        displayText("I'm Ready!", "");
    }
    last = false;
}

// Retrieves a new quote from API, SD card, or EEPROM
void button1() {
    if (settings[1]%2 == 1 && !wifiStatus) {
        WiFi.mode(WIFI_STA);
        bootWiFi(SD_MMC);
    }
    Serial.println("Button 1 Pressed. Getting new quote.");
    String quote = "Error.";
    if (settings[1]%2 == 1) {
        quote = httpRequest("http://quotes.rest/qod.json");    
    }
    if (settings[1] >= 3) {
        quote = readStringFromEEPROM(0);
    } else if (quote.substring(0,6) == "Error."){
        quote = getRandSD(SD_MMC);
    }
    scrollText(settings[6], quote);
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
}

// Displays current time
void button1b() {
    if (settings[1]%2 == 1 && !wifiStatus) {
        WiFi.mode(WIFI_STA);
        bootWiFi(SD_MMC);
    }
    while (digitalRead(BUTTON1) == 0 && digitalRead(BUTTON2) == 0 && showLocalTime(0)){delay(100);}
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
}

// Repeats the previous string written in EEPROM
void button2() {
    Serial.println("Button 2 Pressed. Getting old quote.");
    String quote = readStringFromEEPROM(0);
    scrollText(settings[7], quote);
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
}

// Displays current date
void button2b() {
    if (settings[1]%2 == 1 && !wifiStatus) {
        WiFi.mode(WIFI_STA);
        bootWiFi(SD_MMC);
    }
    while (digitalRead(BUTTON1) == 0 && digitalRead(BUTTON2) == 0 && showLocalTime(1)){delay(100);}
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
}

// Returns whether or not the current time is a valid time to access the API
bool validTime(){
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
        String temp = readStringFromEEPROM(480);
        p_day = temp.substring(0, temp.indexOf(",")).toInt();
        p_hour = temp.substring(temp.indexOf(",") + 1).toInt();
        return (timeinfo.tm_mday != p_day) && (timeinfo.tm_hour >= p_hour);
    }
    return false;
}

// Requests quote from given URL
String httpRequest(String url) {
    String quote = "Error... Contact Larry or wait for a bit!";
    if ((wifiMulti.run() == WL_CONNECTED) && validTime()) {
        HTTPClient http;
        if (http.begin(url)) {
            int httpCode = http.GET();
            if (httpCode > 0) {
                Serial.printf("[HTTP] GET... code: %d\n", httpCode);
                if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
                    String payload = http.getString();
                    quote = getJSONQuote(payload);
                    if (settings[2] == 1) {
                        writeStringToEEPROM(0, quote);
                        struct tm timeinfo;
                        if(getLocalTime(&timeinfo)){
                            String tracker = String(timeinfo.tm_mday) + "," + String(timeinfo.tm_hour);
                            writeStringToEEPROM(480, tracker);
                        }
                    }
                }
            } else {
                Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
            }
            http.end();
        } else {
            Serial.printf("[HTTP} Unable to connect\n");
        }
    }
    return quote;
}

// Retrieves quote from HTTP-returned JSON String
String getJSONQuote(String input) {
    JSONVar myObject = JSON.parse(input);
    String quote = (const char*) myObject["contents"]["quotes"][0]["quote"];
    String auth = (const char*) myObject["contents"]["quotes"][0]["author"];
    return "\"" + quote + "\" - " + auth;
}

// Gets a random quote from SD card's quote folder
String getRandSD(fs::FS &fs) {
    Serial.println("Getting random SD card quote");
    File file = fs.open("/quotes/" + String(random(settings[4], settings[5])) + ".txt");
    String backup = "Something's up with the SD files... Sorry!";
    if (file) {
        backup = "";
        while(file.available()){
            backup += char(file.read());  
        }
        if (settings[2] == 1) {
            writeStringToEEPROM(0, backup);
        }
    }
    return backup;
}

// Commits most recent string to flash
void writeStringToEEPROM(int addrOffset, const String &strToWrite) {
    byte len = strToWrite.length();
    EEPROM.write(addrOffset, len);
    for (int i = 0; i < len; i++) {
        EEPROM.write(addrOffset + 1 + i, strToWrite[i]);
    }
    if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
    } else {
        Serial.println("ERROR! EEPROM commit failed");
    }
}

// Returns most recent string from flash
String readStringFromEEPROM(int addrOffset) {
    int newStrLen = EEPROM.read(addrOffset);
    char data[newStrLen + 1];
    for (int i = 0; i < newStrLen; i++) {
        data[i] = EEPROM.read(addrOffset + 1 + i);
    }
    data[newStrLen] = '\0';
    return String(data);
}

// Displays current time info on screen
bool showLocalTime(int timemode) {
    struct tm timeinfo;
    if(!wifiStatus || (wifiMulti.run() != WL_CONNECTED) || !getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
        displayText("No WiFi...", "");
        delay(2000);
        display.clear();
        return false;
    }
    if (timemode == 0) { // Display time of day
        String ampm = " AM";
        if (timeinfo.tm_hour > 11) {
            ampm = " PM";
        }
        String c_hour = String(timeinfo.tm_hour%12);
        if (c_hour == "0") {
            c_hour = "12";
        }
        String formatted = c_hour + ":" + String(timeinfo.tm_min);
        if (timeinfo.tm_min < 9) {
            formatted = c_hour + ":0" + String(timeinfo.tm_min);
        }
        if (timeinfo.tm_sec > 9) {
            formatted += ":" + String(timeinfo.tm_sec);
        } else {
            formatted += ":0" + String(timeinfo.tm_sec);
        }
        displayText(formatted, ampm);
        return true;
    } else { // Display date
        String date = String(weekdays[timeinfo.tm_wday]) + ", " + String(months[timeinfo.tm_mon]) + " " + String(timeinfo.tm_mday);
        displayText(date, "");
        return true;
    }
    return false;
}

// Displays some Magic 8 answer for short double press
void magic8() {
    while (digitalRead(BUTTON1) == 0 && digitalRead(BUTTON2) == 0){displayText("Magic 8!", "");delay(100);}
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
    String response = responses[random(0, 20)];
    while (digitalRead(BUTTON1) == 0 && digitalRead(BUTTON2) == 0){scrollText(settings[6], response);}
    while (digitalRead(BUTTON1) == 1 || digitalRead(BUTTON2) == 1){display.clear();delay(100);}
}

// Displays text on screen at given speed
void scrollText(int v, String message) {
    Serial.print("Scrolling message: ");
    Serial.println(message);
    display.clear();
    display.setFont(Open_Sans_Condensed_Bold_26);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    int b = -1 * display.getStringWidth(message);
    for (int i = 128; (i > b) && (digitalRead(BUTTON1) == 0 && digitalRead(BUTTON2) == 0); i--) {
        display.drawString(i, 25, message);
        display.display();
        delay(v);
        display.clear();
    }
}

// Displays text on screen, centered
void displayText(String message, String ampm) {
    display.clear();
    display.setFont(Open_Sans_Condensed_Bold_26);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    if (ampm.length() == 0) {
        if (display.getStringWidth(message) > 128) {
            display.setFont(Open_Sans_Condensed_Bold_24);
            if (display.getStringWidth(message) > 128) {
                display.setFont(Open_Sans_Condensed_Bold_20);
            }
        }
        int b = 64 - (display.getStringWidth(message) / 2);
        display.drawString(b, 23, message);
        display.display();   
    } else {
        int len = display.getStringWidth(message);
        int b = 64 - ((len + 28) / 2);
        display.drawString(b, 23, message);
        display.setFont(Open_Sans_Condensed_Bold_20);
        display.drawString(b + len, 25, ampm);
        display.display();
    }
}
