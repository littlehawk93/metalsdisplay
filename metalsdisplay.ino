#include <TM1637TinyDisplay6.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>

// Timing definitions
#define DURATION_LED_REFRESH_MILLIS 20 // 20 ms = 50 display refreshes per second
#define DURATION_API_REFRESH_MILLIS 600000 // 600,000 ms = 10 minutes
#define DURATION_BLINK_MILLIS 250 // 1/4 second blink rate (1/4 LED off, 1/4 LED on)
#define DURATION_BUTTON_IGNORE_MILLIS 200 // delay between registering button presses, helps reduce "double press"

// Pin definitions
#define PIN_SPI_MOSI 23
#define PIN_SPI_CLK 18
#define PIN_DISPLAY_CLK 26
#define PIN_DISPLAY_DATA 25
#define PIN_INTERRUPT_BUTTON 21
#define PIN_SDCS 5
#define PIN_LED_SILVER 4
#define PIN_LED_GOLD 22

// Config variables read from SD card
char *webaddress; // Host name of the server to connect to
char *ssid; // SSID of Wi-Fi network to connect to
char *password; // Password of Wi-Fi network to connect to

double goldValue = 0.0;
double silverValue = 0.0;

// Flag for determining whether gold or silver is displayed
bool showGold = true;

// Flag for determining if blinking LED is on (HIGH) or off (LOW)
bool ledHigh = false;

// Timing variables. Allows processing to continue without using delays
unsigned long lastLEDRefreshMillis = 0;
unsigned long lastAPIRefreshMillis = 0;
unsigned long lastButtonPressMillis = 0;
unsigned long lastBlinkEventMillis = 0;

volatile unsigned long currentMillis = 0;

WiFiClient client;
TM1637TinyDisplay6 display(PIN_DISPLAY_CLK, PIN_DISPLAY_DATA);

/*
 * Interrupt Function. When the button is pressed, toggle between displaying the current Gold price or the current Silver price
 */
void IRAM_ATTR buttonPressed() {

    if (currentMillis < lastButtonPressMillis || currentMillis - lastButtonPressMillis > DURATION_BUTTON_IGNORE_MILLIS) {
        if(showGold) {
            showGold = false;
            digitalWrite(PIN_LED_GOLD, LOW);
            digitalWrite(PIN_LED_SILVER, HIGH);
        } else {
            showGold = true;
            digitalWrite(PIN_LED_SILVER, LOW);
            digitalWrite(PIN_LED_GOLD, HIGH);
        }
        lastButtonPressMillis = currentMillis;
    }
}

void setup()
{
    Serial.begin(115200);

    // Configure TM1637 display pins and clear the display
    pinMode(PIN_DISPLAY_DATA, OUTPUT);
    pinMode(PIN_DISPLAY_CLK, OUTPUT);

    display.setBrightness(BRIGHT_HIGH, true);
    display.clear();

    // Configure remaining pins
    pinMode(PIN_SDCS, OUTPUT);
    pinMode(PIN_INTERRUPT_BUTTON, INPUT);
    pinMode(PIN_LED_SILVER, OUTPUT);
    pinMode(PIN_LED_GOLD, OUTPUT);

    // Gold LED is on until configuration file is fully read
    digitalWrite(PIN_LED_GOLD, HIGH);
    digitalWrite(PIN_LED_SILVER, LOW);

    readConfig();

    // Silver LED blinks on and off until connected to Wi-Fi network
    digitalWrite(PIN_LED_GOLD, LOW);
    digitalWrite(PIN_LED_SILVER, HIGH);

    WiFi.mode(WIFI_MODE_STA);
    WiFi.setTxPower(WIFI_POWER_19dBm);
    WiFi.begin(ssid, password);

    // Wait until Wi-Fi status is "connected"
    while (WiFi.status() != WL_CONNECTED) {
      currentMillis = millis();

      // Toggle the blinking LED on / off at the designated blink rate
      if(currentMillis < lastBlinkEventMillis || currentMillis - lastBlinkEventMillis > DURATION_BLINK_MILLIS) {
        ledHigh = !ledHigh;
        lastBlinkEventMillis = currentMillis;
        digitalWrite(PIN_LED_SILVER, ledHigh ? HIGH : LOW);
      }

      // prevent spamming the Wi-Fi connection status
      delay(50);
    }

    // Revert back to Gold on (indicating Gold price is displayed)
    digitalWrite(PIN_LED_GOLD, HIGH);
    digitalWrite(PIN_LED_SILVER, LOW);

    // Attach the button interrupt pin so that button presses are registered
    attachInterrupt(PIN_INTERRUPT_BUTTON, buttonPressed, RISING);
}

/*
 * Main logic loop
 * checks timing flags to 
 */
void loop()
{
    currentMillis = millis();

    // If enough time has passed since the last API refresh, call the Gold & Silver API to get updated values
    if (lastAPIRefreshMillis == 0 || currentMillis < lastAPIRefreshMillis || currentMillis - lastAPIRefreshMillis > DURATION_API_REFRESH_MILLIS) {
        getMetalValues();
        lastAPIRefreshMillis = currentMillis;
    }

    // Refresh display value if enough time has passed
    if (currentMillis < lastLEDRefreshMillis || currentMillis - lastLEDRefreshMillis > DURATION_LED_REFRESH_MILLIS) {
        display.showNumberDec((showGold ? goldValue : silverValue) * 100, 0x10, false, 6, 0);
        lastLEDRefreshMillis = currentMillis;
    }

    // slight delay between cycles to save power
    delay(5);
}

/*
 * Uses Wi-Fi client to read data from Gold & Silver API.
 */
void getMetalValues()
{
    if(client.connect(webaddress, 80))
    {
        client.printf("GET /metals.php?format=JSON HTTP/1.1\r\n", webaddress);
        client.printf("Host: %s\r\n", webaddress);
        client.println("Connection: close\r\n\r\n");
        client.println();

        while (client.available() == 0);

        readApiResponse(client);
    }
    else
    {
        Serial.printf("Failed to connect to %s\n", webaddress);
    }

    client.stop();
}

/*
 * Parses API response to set the Gold and Silver value variables
 * Basic JSON parser. This is not a full implementation of JSON protocol, but will suffice for basic response object
 * 
 * Expected API response format: {"gold":1000.00,"silver":25.00}
 */
void readApiResponse(WiFiClient client)
{
    goldValue = -1.0;
    silverValue = -1.0;

    bool inObject = false;
    bool inKey = false;
    bool inValue = false;

    int valueIndex = 0;
    int keyIndex = 0;

    char key[50];
    char value[20];

    int c;

    while((c = client.read()) != -1)
    {
        if (!inObject && c == '{')
        {
            inObject = true;
        }
        else if (inObject)
        {
            if(c == '}')
            {
                value[valueIndex] = '\0';
                inObject = false;

                if (strcmp(key, "gold") == 0)
                {
                    goldValue = atof(value);
                }
                else if (strcmp(key, "silver") == 0)
                {
                    silverValue = atof(value);
                }
                return;
            }
            else if (!inKey && c == '"')
            {
                inKey = true;
                keyIndex = 0;
            }
            else if (inKey)
            {
                if(c == '"')
                {
                    inKey = false;
                    key[keyIndex] = '\0';
                }
                else if (keyIndex < 49)
                {
                    key[keyIndex] = (char)c;
                    keyIndex++;
                }
            }
            else if (!inValue && c == ':')
            {
                inValue = true;
                valueIndex = 0;
            }
            else if (inValue)
            {
                if (c == ',')
                {
                    value[valueIndex] = '\0';
                    inValue = false;

                    if (strcmp(key, "gold") == 0)
                    {
                        goldValue = atof(value);
                    }
                    else if (strcmp(key, "silver") == 0)
                    {
                        silverValue = atof(value);
                    }
                }
                else if (valueIndex < 19)
                {
                    value[valueIndex] = (char)c;
                    valueIndex++;
                }
            }
        }
    }
}

/*
 * Reads config file from the attached SD card and saves the parsed values into the config variables: webaddress, ssid, and password
 * 
 * Config file is expected to be in INI file format:
 * KEY=VALUE
 * KEY2=VALUE2
 */
void readConfig() 
{
    digitalWrite(PIN_SDCS, LOW);

    if (!SD.begin(PIN_SDCS)) {
        Serial.println("Failed to open SD card");
        ssid = "";
        password = "";
        webaddress = "";
        return;
    }

    File configFile = SD.open("/CONFIG.INI");

    if (!configFile) {
        Serial.println("Failed to open config file");
        ssid = "";
        password = "";
        webaddress = "";
        return;
    }

    char key[100];
    char value[100];

    while(readLine(configFile, key, value))
    {
        int len = strlen(value)+1;

        if(strcmp("SSID", key) == 0)
        {
            ssid = (char*)malloc(sizeof(char)*len);

            for(int i=0;i<len;i++) {
                ssid[i] = value[i];
            }
        }
        else if (strcmp("PWD", key) == 0)
        {
            password = (char*)malloc(sizeof(char)*len);

            for(int i=0;i<len;i++) {
                password[i] = value[i];
            }
        }
        else if(strcmp("ADDRESS", key) == 0)
        {
            webaddress = (char*)malloc(sizeof(char)*len);

            for(int i=0;i<len;i++) {
                webaddress[i] = value[i];
            }
        }
    }

    configFile.close();
    SD.end();

    digitalWrite(PIN_SDCS, HIGH);
}

/*
 * Parse a single line from an INI formatted file. Reads the line and stores the results in the provided key and value string variables
 */
bool readLine(File file, char *key, char *value)
{
    int keyIndex = 0;
    int valueIndex = 0;

    bool readKey = true;
    int c;

    while ((c = file.read()) != -1)
    {
        if(c == '\n')
        {
            key[keyIndex] = '\0';
            value[valueIndex] = '\0';
            return true;
        }
        else if (c == '=')
        {
            readKey = false;
            continue;
        }
        else if (readKey && keyIndex < 99 && c >= 32 && c < 127)
        {
            key[keyIndex] = (char)c;
            keyIndex++; 
        }
        else if (!readKey && valueIndex < 99 && c >= 32 && c < 127)
        {
            value[valueIndex] = (char)c;
            valueIndex++;
        }
    }

    key[keyIndex] = '\0';
    value[valueIndex] = '\0';
    return false;
}
