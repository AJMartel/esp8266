//
//   Helsinki Tram-Departure Display - https://steve.fi/Hardware/
//
//   This is a simple program which uses WiFi & an LCD display
//   to show useful information.
//
//   The top line of the display will default to showing the current time,
//   and date.  But it might display a fixed message, or the temperature
//   instead.
//
//   Each additional line of the display will show the departure
//   of the next tram from the chosen stop.  For example we'd
//   we would see something like this on a 4x20 LCD display:
//
//      #######################
//      # 11:12:13 Fri 21 Apr #
//      #   Line 4B @ 11:17   #
//      #   Line 7B @ 11:35   #
//      #   Line 10 @ 11:45   #
//      #######################
//
//
//   There is a simple HTTP-server which is running on port 80, which
//   will mirror the LCD-contents, and allow changes to be made to the
//   device.
//
//   For example the user can change the time-zone offset, the tram-stop,
//   toggle the backlight, or even change the remote URL being polled.
//
//   The HTTP-server also allows changing the display-mode too.
//
//   The user-selectable-changes will be persisted to flash memory, so
//   they survive reboots.
//
//   Because the Helsinki Tram API is a complex GraphQL-beast we're getting
//   data via a simple helper:
//
//       https://api.steve.fi/Helsinki-Transport/
//
//   We use a similar site to get the temperature, if that is selected as
//   a display mode (via the web-UI):
//
//       https://api.steve.fi/Helsinki-Temperature/
//
//
//  Optional Button
//  --------------
//
//    If you wire a button between D0 & D8 you can control the device
//    using it:
//
//      Single press & release
//         Toggle the backlight.
//
//      Double-Click
//         Show IP and other data.
//
//      Long-press & release
//         Resync time & departure-data.
//
//
//   Steve
//   --
//


//
// The name of this project.
//
// Used for the Access-Point name, and for OTA-identification.
//
#define PROJECT_NAME "TRAM-TIMES"


//
// The number of rows/columns of our display.
//
#define NUM_ROWS 4
#define NUM_COLS 20


//
// The URL to fetch our tram-data from.
//
// The string `__ID__` in this URL will be replaced by the ID of the stop.
//
#define DEFAULT_API_ENDPOINT "http://api.steve.fi/Helsinki-Transport/data/__ID__"


//
// The temperature API URL
//
#define DEFAULT_WEATHER_ENDPOINT "http://api.steve.fi/Helsinki-Temperature/data/"


//
// The default stop to monitor.
//
#define DEFAULT_TRAM_STOP "1160404"


//
// For WiFi setup.
//
#include "WiFiManager.h"


//
// We persist our settings to flash, so we need this.
//
#include <FS.h>


//
// WiFi & over the air updates
//
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>


//
// For driving the LCD
//
#include <Wire.h>
#include "LiquidCrystal_I2C.h"


//
// For dealing with NTP & the clock.
//
#include "NTPClient.h"


//
// Debug messages over the serial console.
//
#include "debug.h"


//
// The button handler
//
#include "OneButton.h"


//
// For fetching URLS & handling URL-parameters
//
#include "url_fetcher.h"
#include "url_parameters.h"


//
// Forward-declaration of function-prototypes.
//
String read_file(const char *path);
void draw_line(int row, const char *txt);
void access_point_callback(WiFiManager* myWiFiManager);
void on_before_ntp();
void on_after_ntp();
void fetch_tram_times();
void handlePendingButtons();
void on_short_click();
void on_long_click();
void on_double_click();
void processHTTPRequest(WiFiClient client);
void set_display_mode(const char *mode);

//
// NTP client, and UDP socket it uses.
//
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);


//
// Timezone offset from GMT
//
int time_zone_offset = 0;


//
// The HTTP-server we present runs on port 80.
//
WiFiServer server(80);


//
// The purpose of our project is to display tram/bus departures from
// a given stop.  However we also show the time/date upon the first
// screen.
//
// There are a couple of other display-modes which may be selected:
//
//  Time & date.
//  Time & temperature.
//  Time & date/temperature - swapping every ten seconds.
//  A fixed message.
//
// Here we describe those possible states
//
typedef enum {DATE, DATE_OR_TEMP, MESSAGE, TEMPERATURE} state;

//
// Our currently-selected display-mode.
//
state g_state = DATE;


//
// Our current temperature, as fetched via the remote API.
//
char g_temp[10] = {'\0'};

//
// Our current message, if any, which has been set by a HTTP-client
//
char g_msg[128] = { '\0' };

//
// The ID of the bus/tram stop we're going to display departures for.
//
char tram_stop[12] = { '\0' };

//
// The API end-point to return departure-data from.
//
char api_end_point[256] = { '\0' };

//
// The API end-point for getting temperature.
//
char temp_end_point[256] = { '\0' };


//
// This two-dimensional array holds the text that we're
// going to display upon our LCD.
//
// The first line will ALWAYS be the time/date, or the other
// data depending upon the display-mode.
//
// Any additional lines will contain the departure
// time of the next tram(s).
//
char screen[NUM_ROWS][NUM_COLS];


//
// Set the LCD address to 0x27, and define the number
// of rows & columns.
//
LiquidCrystal_I2C lcd(0x27, NUM_COLS, NUM_ROWS);

//
// Is the backlight lit?  Defaults to true.
//
bool backlight = true;

//
// Time for the backlight to go on/off automatically
//
// This is set via the user-interface, and is optional.
//
int backlight_on = -1;
int backlight_off = -1;


//
// Setup a new OneButton on pin D8.
//
OneButton button(D8, false);


//
// Are there pending clicks to process?
//
volatile bool short_click = false;
volatile bool double_click = false;
volatile bool long_click = false;


//
// This function is called when the device is powered-on.
//
void setup()
{
    //
    // Enable our serial port.
    //
#ifdef DEBUG
    Serial.begin(115200);
#endif

    //
    // Enable access to the filesystem.
    //
    SPIFFS.begin();

    //
    // Load the tram-stop if we can
    //
    String tram_stop_str = read_file("/tram.stop");

    if (tram_stop_str.length() > 0)
        strncpy(tram_stop, tram_stop_str.c_str(), sizeof(tram_stop) - 1);
    else
        strncpy(tram_stop, DEFAULT_TRAM_STOP, sizeof(tram_stop) - 1);

    //
    // Load the API end-point if we can
    //
    String api_end_str = read_file("/tram.api");

    if (api_end_str.length() > 0)
        strncpy(api_end_point, api_end_str.c_str(), sizeof(api_end_point) - 1);
    else
        strncpy(api_end_point, DEFAULT_API_ENDPOINT, sizeof(api_end_point) - 1);

    //
    // Load the temperature-endpoint, if we can.
    //
    String weather_end = read_file("/temp.api");

    if (weather_end.length() > 0)
        strncpy(temp_end_point, weather_end.c_str(), sizeof(temp_end_point) - 1);
    else
        strncpy(temp_end_point, DEFAULT_WEATHER_ENDPOINT, sizeof(temp_end_point) - 1);

    //
    // Load the time-zone offset, if we can
    //
    String tz_str = read_file("/time.zone");

    if (tz_str.length() > 0)
        time_zone_offset = tz_str.toInt();


    //
    // Load our display-mode, if we can
    //
    String md = read_file("/display.mode");

    if (md.length() > 0)
        set_display_mode(md.c_str());

    //
    // Load our message, if we can.
    //
    String msg = read_file("/text.msg");

    if (msg.length() > 0)
    {
        strncpy(g_msg, msg.c_str(), sizeof(g_msg) - 1);
    }

    //
    // If we have a schedule for the backlight then load
    // that here too.
    //
    // That involves two times; one to go off and one to
    // go on.
    //
    String bon = read_file("/b.on");

    if (bon.length() > 0)
    {
        backlight_on = atoi(bon.c_str());
        DEBUG_LOG("Backlight schedule turns on at %d\n", backlight_on);
    }

    String boff = read_file("/b.off");

    if (boff.length() > 0)
    {
        backlight_off = atoi(boff.c_str());
        DEBUG_LOG("Backlight schedule turns off at %d\n", backlight_off);
    }

    //
    // initialize the LCD
    //
    lcd.begin();
    lcd.setBacklight(true);

    //
    // Show a message.
    //
    draw_line(0, "Starting up ..");

    //
    // Handle the connection to the local WiFi network.
    //
    WiFiManager wifiManager;
    wifiManager.setAPCallback(access_point_callback);
    wifiManager.autoConnect(PROJECT_NAME);

    //
    // Now we're connected show the local IP address.
    //
    draw_line(0, "WiFi Connected");
    draw_line(1, WiFi.localIP().toString().c_str());

    //
    // Allow the IP to be visible.
    //
    delay(1500);

    //
    // Ensure our NTP-client is ready.
    //
    timeClient.begin();

    //
    // Configure the NTP-callbacks.
    //
    timeClient.on_before_update(on_before_ntp);
    timeClient.on_after_update(on_after_ntp);

    //
    // Setup the timezone & update-interval.
    //
    timeClient.setTimeOffset(time_zone_offset * (60 * 60));
    timeClient.setUpdateInterval(300 * 1000);

    //
    // Now we can start our HTTP server
    //
    server.begin();
    DEBUG_LOG("HTTP-Server started on http://%s/\n",
              WiFi.localIP().toString().c_str());

    //
    // Allow over the air updates.
    //
    ArduinoOTA.setHostname(PROJECT_NAME);

    ArduinoOTA.onStart([]()
    {
        draw_line(0, "OTA Start");
    });
    ArduinoOTA.onEnd([]()
    {
        draw_line(0, "OTA Ended");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
    {
        char buf[NUM_COLS + 1];
        memset(buf, '\0', sizeof(buf));
        snprintf(buf, NUM_COLS, "Upgrade - %02u%%", (progress / (total / 100)));
        draw_line(0, buf);
    });
    ArduinoOTA.onError([](ota_error_t error)
    {
        if (error == OTA_AUTH_ERROR)
            draw_line(0, "Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
            draw_line(0, "Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
            draw_line(0, "Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
            draw_line(0, "Receive Failed");
        else if (error == OTA_END_ERROR)
            draw_line(0, "End Failed");
    });

    //
    // Ensure the OTA process is running & listening.
    //
    ArduinoOTA.begin();

    //
    // We support the use of an optional switch between D8 & D0.
    //
    // Configure that here.
    //
    pinMode(D8, INPUT_PULLUP);
    pinMode(D0, OUTPUT);
    digitalWrite(D0, HIGH);

    //
    // Configure the button-action(s).
    //
    button.attachClick(on_short_click);
    button.attachDoubleClick(on_double_click);
    button.attachLongPressStop(on_long_click);

}


//
// Record that a short-click happened.
//
void on_short_click()
{
    short_click = true;
}


//
// Record that a double-click happened.
//
void on_double_click()
{
    double_click = true;
}

//
// Record that a long-click happened.
//
void on_long_click()
{
    long_click = true;
}

//
// Called just before the date/time is updated via NTP
//
void on_before_ntp()
{
    draw_line(NUM_ROWS - 1, "Updating date & time");
}

//
// Called just after the date/time is updated via NTP
//
void on_after_ntp()
{
    draw_line(NUM_ROWS - 1, "Updated date & time");
}

//
// If we're unconfigured we run an access-point.
//
// Show that, explicitly.
//
void access_point_callback(WiFiManager* myWiFiManager)
{
    draw_line(0, "AccessPoint Mode");
    draw_line(1, PROJECT_NAME);

    if (NUM_ROWS > 2)
    {
        draw_line(2, "IP Address:");
        draw_line(3, WiFi.localIP().toString().c_str());
    }
}



//
// This function is called continuously.
//
void loop()
{
    //
    // Keep the previous time, to avoid needless re-draws
    //
    static char prev_time[NUM_COLS] = { '\0'};

    //
    // If we're in the date/temperature mode we need to keep track
    // of the last time we swapped state, and what state we're currently
    // in.
    //
    static long last_change = millis();
    static state g_temp_date = DATE;

    //
    // If we're displaying a fixed-message, instead of the time/similar
    // then we might need to handle scrolling it - if it is longer than
    // our display-width.
    //
    // Keep track of the offset here.
    //
    static int msg_offset = 0;

    //
    // Handle any pending over the air updates.
    //
    ArduinoOTA.handle();

    //
    // Resync the clock.
    //
    timeClient.update();

    //
    // Process our button - looking for clicks, double-clicks, & long-clicks
    //
    button.tick();

    //
    // Handle any pending clicks here.
    //
    handlePendingButtons();

    //
    // Get the current time.
    //
    // We save this, so that we can trigger actions such as updating
    // the departure-data, and reloading the temperature data.
    //
    int hour = timeClient.getHours();
    int min  = timeClient.getMinutes();
    int sec  = timeClient.getSeconds();
    int year = timeClient.getYear();
    String d_name = timeClient.getWeekDay();
    String m_name = timeClient.getMonth();
    int day = timeClient.getDayOfMonth();


    //
    // Every two minutes we'll update the departure times.
    //
    // We also do it immediately the first time we're run,
    // when there is no pending time available.
    //
    if ((strlen(screen[1]) == 0) || ((min % 2 == 0) && (sec == 0)))
        fetch_tram_times();

    //
    // Every half hour we'll update the temperature, or initially if empty.
    //
    // Note that we don't bother unless we're in a mode where the
    // temperature might be displayed.
    //
    if (g_state == TEMPERATURE || g_state == DATE_OR_TEMP)
        if ((strlen(g_temp) == 0) || ((min % 30 == 0) && (sec == 0)))
            fetch_temperature();


    //
    // Every 10 seconds we swap between date + temp if either is displayed.
    //
    // NOTE: We have to record the time of the last change here
    // because otherwise this loop might come around while the
    // second value hasn't changed.  That would result in the
    // mode being changed N times in a second, with much confusion
    // and hilarity.
    //
    // Of course this only makes sense if we have a temperatur-end-point
    // setup, so ignore this if we don't.
    //
    if ((g_state == DATE_OR_TEMP) && (strlen(temp_end_point) > 0))
    {
        if (((sec % 10) == 0) && ((millis() - last_change) > 1000))
        {
            switch (g_temp_date)
            {
            case DATE:
                g_temp_date = TEMPERATURE;
                break;

            case TEMPERATURE:
                g_temp_date = DATE;
                break;

            }

            // Record the last time we transitioned state.
            last_change = millis();
        }
    }


    //
    // Clear the first row of the display.
    //
    memset(screen[0], '\0', NUM_COLS);

    //
    // Decide what to draw.
    //
    switch (g_state)
    {
    case DATE:
        snprintf(screen[0], NUM_COLS, "%02d:%02d:%02d %s %02d %s %04d",
                 hour, min, sec, d_name.c_str(), day, m_name.c_str(), year);
        break;

    case TEMPERATURE:
        snprintf(screen[0], NUM_COLS, "%02d:%02d:%02d %s",
                 hour, min, sec, g_temp);
        break;

    case DATE_OR_TEMP:

        // repetition here is bad
        switch (g_temp_date)
        {
        case DATE:
            snprintf(screen[0], NUM_COLS, "%02d:%02d:%02d %s %02d %s %04d",
                     hour, min, sec, d_name.c_str(), day, m_name.c_str(), year);
            break;

        case TEMPERATURE:
            snprintf(screen[0], NUM_COLS, "%02d:%02d:%02d %s",
                     hour, min, sec, g_temp);
            break;
        }

        break;

    case MESSAGE:

        // Drawing a fixed message.  There are two cases, where
        // the text is "short" and fits, and when the text must be
        // scrolled.
        int len = strlen(g_msg);

        if (len < NUM_COLS)
        {
            snprintf(screen[0], NUM_COLS - 1, "%s", g_msg);
        }
        else
        {
            // Draw the message.
            memset(screen[0], '\0', NUM_COLS);
            snprintf(screen[0], NUM_COLS - 1, "%s", g_msg + msg_offset);

            // Ensure that we absolutely, definitely, null-terminate it.
            screen[0][NUM_COLS - 1] = '\0';

            // Bump the offset for next time we redraw the text.
            if ((millis() - last_change) > 333)
            {
                msg_offset += 1;

                // but don't walk off the end of the string.
                if (msg_offset >= len)
                    msg_offset = 0;

                last_change = millis();
            }
        }

        break;
    }


    //
    // Now draw all the rows - correctly doing this
    // after the previous steps might have updated
    // the display.
    //
    // That avoids showing outdated information, albeit
    // information that is only outdated for <1 second!
    //
    // NOTE: We delay() for less than a second in this function
    // so we cheat here, only updating the LCD if the currently
    // formatted time is different to that we set in the past.
    //
    // i.e. We don't re-draw the screen unless the first-line has changed.
    //
    // If the real-time hasn't changed then the tram-arrival times
    // haven't changed either, since that only updates every two minutes.
    //
    if (strcmp(prev_time, screen[0]) != 0)
    {
        for (int i = 0; i < NUM_ROWS; i++)
            draw_line(i, screen[i]);

        //
        // Keep the current time in our local/static buffer.
        //
        strcpy(prev_time, screen[0]);
    }


    //
    // Optionally we allow disabling the backlight on a schedule.
    //
    // We test for this every hour, on the hour.  If a schedule hasn't
    // been setup then the `on` and `off` times will be `-1`, so
    // they will never match and we're safe.
    //
    if ((min == 0) && (sec == 0))
    {
        // The time for it to go on.
        if (backlight_on == hour)
        {
            if (! backlight)
            {
                backlight = true;
                lcd.setBacklight(backlight);
            }
        }

        // The time for it to go off
        if (backlight_off == hour)
        {
            if (backlight)
            {
                backlight = false;
                lcd.setBacklight(backlight);
            }
        }
    }


    //
    // Check if a client has connected to our HTTP-server.
    //
    // If so handle it.
    //
    // (This allows changing the stop, timezone, backlight, etc.)
    //
    WiFiClient client = server.available();

    if (client)
        processHTTPRequest(client);

    //
    // Now sleep a little.
    //
    delay(10);
}

//
// Set the display-mode appropriately, given a string describing
// the desired state.
// This function might be called as a result of a HTTP-POST, or
// by reading the mode on-startup.
//
void set_display_mode(const char *mode)
{
    if (strcmp(mode, "date") == 0)
    {
        g_state = DATE;
    }

    if (strcmp(mode, "temp") == 0)
    {
        g_state = TEMPERATURE;
    }

    if (strcmp(mode, "dt") == 0)
    {
        g_state = DATE_OR_TEMP;
    }

    if (strcmp(mode, "msg") == 0)
    {
        g_state = MESSAGE;
    }
}

//
// We bind our button such that short-clicks, long-clicks,
// and double-clicks will invoke a call-back.
//
// The callbacks just record the pending state, and here we
// process any of them that were raised.
//
void handlePendingButtons()
{

    //
    // If we have a pending-short-click then handle it
    //
    if (short_click)
    {
        short_click = false;
        DEBUG_LOG("Short Click\n");


        // Toggle the state of the backlight
        backlight = !backlight;
        lcd.setBacklight(backlight);
    }

    //
    // If we have a pending long-click then handle it.
    //
    if (long_click)
    {
        long_click = false;
        DEBUG_LOG("Long Click\n");

        // Update the date/time
        timeClient.forceUpdate();

        // Refresh the tram data
        fetch_tram_times();

        // Refresh the temperature
        if (g_state == TEMPERATURE || g_state == DATE_OR_TEMP)
            fetch_temperature();

    }

    //
    // If we have a pending double-click then handle it
    //
    if (double_click)
    {

        double_click = false;
        DEBUG_LOG("Double Click\n");

        char line[NUM_COLS + 1];

        // Line 0 - About
        draw_line(0, "Steve Kemp - Trams");

        // Line 1 - IP
        snprintf(line, NUM_COLS, "IP: %s", WiFi.localIP().toString().c_str());
        draw_line(1, line);

        // Line 2 - Timezone
        if (time_zone_offset > 0)
            snprintf(line, NUM_COLS, "Timezone: +%d", time_zone_offset);
        else if (time_zone_offset < 0)
            snprintf(line, NUM_COLS, "Timezone: -%d", time_zone_offset);
        else
            snprintf(line, NUM_COLS, "Timezone: %d", time_zone_offset);

        draw_line(2, line);

        // Line 3 - Tram ID
        snprintf(line, NUM_COLS, "Tram ID : %s", tram_stop);
        draw_line(3, line);

        delay(2500);

    }


}




//
// Given a bunch of CSV, containing departures, we parse this and
// update the screen-array.
//
// We handle CSV of the form:
//
//    NNNNN,HH:MM:SS,Random-Text
//
// The first field is the tram/bus ID, the second is the time in
// full hour-minute-second format, and the third is the name of the
// route.
//
// We'll cope with a bus/tram-ID of up to five characters, and we'll
// keep the display neat by truncating the time at HH:MM.
//
// The name of the route is not displayed.
//
void update_tram_times(const char *txt)
{
    char* pch = NULL;

    int line = 1;
    pch = strtok((char *)txt, "\r\n");

    while (pch != NULL)
    {
        //
        // If we got a line, and it is at least ten characters
        // long then it is probably valid.
        //
        // The line will be:
        //
        //   NN,HH:MM:SS,NAME
        //
        // So if we assume a two-digit ID such as "10", "7A", "4B",
        // and the six digits of the time, then ten is a reasonable
        // bound on the minimum-length of a valid-line.
        //
        if ((strlen(pch) > 10) && (line < NUM_ROWS))
        {

            //
            // Skip newlines / carriage returns
            //
            while (pch[0] == '\n' || pch[0] == '\r')
                pch += 1;

            //
            // Look for the first comma, which seperates
            // the tram/bus-number and the time.
            //
            // We don't know how long that bus/tram ID
            // will be.  But we'll assume <=6 characters
            // later on.
            //
            char *comma = strchr(pch, ',');

            //
            // If we found a comma then proceed.
            //
            if (comma != NULL)
            {
                // ID of line, and time of departure.
                char id[6] = {'\0'};
                char tm[6] = {'\0'};

                //
                // So our line-ID is contained between pch & comma.
                //
                // Copy it, capping it if we need to at five characters.
                //
                memset(id, '\0', sizeof(id));

                strncpy(id, pch, (comma - pch) >= sizeof(id) ? sizeof(id) - 1 : (comma - pch));

                //
                // Now we have comma pointing to ",HH:MM:SS,DESCRIPTION-HERE"
                //
                // We want to extract the time, and save that away.
                //
                // If our time is HH:MM:SS then c + 9 will be a comma
                //
                // We will copy just the HH:MM part of the time, so five
                // digits in total.
                //
                if (comma[9] == ',')
                    strncpy(tm, comma + 1, 5);

                snprintf(screen[line], NUM_COLS - 1, "  Line %s @ %s", id, tm);

            }

            //
            // Bump to the next display-line
            //
            line += 1;
        }

        pch = strtok(NULL, "\r\n");
    }
}


//
// Call a remote HTTP-service to get the current temperature.
//
void fetch_temperature()
{
    draw_line(NUM_ROWS - 1, "Refreshing temp ..");

    // Empty the previous value.
    memset(g_temp, '\0', sizeof(g_temp));

    //
    // Make our remote call.
    //
    DEBUG_LOG("Fetching temperature-data from %s\n", temp_end_point);
    UrlFetcher client(temp_end_point);

    //
    // If that succeeded.
    //
    int code = client.code();

    if (code == 200)
    {
        //
        // Parse the returned data and process it.
        //
        String body = client.body();

        if (body.length() > 0)
        {
            // "degree", "C", "terminator".
            char deg[3] = { 0xDF, 'C', '\0' };

            // Remove the any newline(s) that might be present in the
            // response from the remote host.
            body.trim();

            // Update `g_temp` with the returned temperature,
            // appending "$degree C".
            snprintf(g_temp, sizeof(g_temp) - 1, "%s%s", body.c_str(), deg);
        }
        else
        {
            DEBUG_LOG("Empty body received.\n");
            strcpy(g_temp, "TFAIL1");
        }
    }
    else
    {
        DEBUG_LOG("HTTP-Request failed, status-Code was %03d\n", code);
        DEBUG_LOG("Status line read: '%s'\n", client.status());
        snprintf(g_temp, sizeof(g_temp) - 1, "TFAIL%d", code);
    }
}


//
// Call our HTTP-service and retrieve the tram time(s).
//
// This function will update the global "screen" array
// with the departure time of the next tram(s).
//
void fetch_tram_times()
{
    //
    // Show what we're doing on the last row.
    //
    draw_line(NUM_ROWS - 1, "Refreshing Trams ..");

    //
    // The URL we're going to fetch, replacing `__ID__` with
    // the ID of the tram.
    //
    String url(api_end_point);
    url.replace("__ID__", tram_stop);

    //
    // Show what we're going to do.
    //
    DEBUG_LOG("Fetching tram-data from %s\n", url.c_str());

    //
    // Fetch the contents of the remote URL.
    //
    UrlFetcher client(url.c_str());

    //
    // If that succeeded.
    //
    int code = client.code();

    if (code == 200)
    {

        //
        // Parse the returned data and process it.
        //
        String body = client.body();

        if (body.length() > 0)
        {
            update_tram_times(body.c_str());
        }
        else
        {
            DEBUG_LOG("Empty response from HTTP-fetch\n");
            strncpy(screen[1], "Empty HTTP response.", NUM_COLS - 1);
            strncpy(screen[2], "Replacement bus?", NUM_COLS - 1);
        }
    }
    else
    {
        //
        // Log the status-code
        //
        DEBUG_LOG("HTTP-Request failed, status-code was %03d\n", code);
        strncpy(screen[1], "HTTP failure", NUM_COLS - 1);

        //
        // Log the status-line
        //
        DEBUG_LOG("Status line read: '%s'\n", client.status());
        strncpy(screen[2], client.status(), NUM_COLS - 1);
    }
}


//
// Serve a redirect to the server-root
//
void redirectIndex(WiFiClient client)
{
    client.println("HTTP/1.1 302 Found");
    client.print("Location: http://");
    client.print(WiFi.localIP().toString().c_str());
    client.println("/");
}


//
// Output a <select> tag, with a list of hours.
//
// One of these might be selected.
//
void output_select(WiFiClient client, char *name, bool enabled, int selected)
{
    client.printf("<select id=\"%s\" name=\"%s\" %s>", name, name,
                  enabled ? "" : "disabled");

    for (int i = 0; i < 24; i++)
    {
        client.printf("<option value=\"%02d\"%s>%02d</option>",
                      i, selected == i ? " selected=\"selected\"" : "", i);
    }

    client.println("</select>");
}

//
// This is a bit horrid.
//
// Serve a HTML-page to any clients who connect via a browser.
//
void serveHTML(WiFiClient client)
{
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("");


    client.println("<!DOCTYPE html>");
    client.println("<html lang=\"en\">");
    client.println("<head>");
    client.println("<title>Tram Times</title>");
    client.println("<meta charset=\"utf-8\">");
    client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
    client.println("<link rel=\"stylesheet\" href=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css\">");
    client.println("<script src=\"https://ajax.googleapis.com/ajax/libs/jquery/3.3.1/jquery.min.js\"></script>");
    client.println("<script src=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js\"></script>");
    client.println("<style>");
    client.println("blockquote { border-left: 0 }");
    client.println(".underline {width:100%; border-bottom: 1px solid grey;}");
    client.println("</style>");
    client.println("<script>");
    client.println("$(function(){");
    client.println("var hash = window.location.hash;");
    client.println("hash && $('ul.nav a[href=\"' + hash + '\"]').tab('show');");
    client.println("$('.nav-tabs a').click(function (e) {");
    client.println("$(this).tab('show');");
    client.println("var scrollmem = $('body').scrollTop() || $('html').scrollTop();");
    client.println("window.location.hash = this.hash;");
    client.println("$('html,body').scrollTop(scrollmem);");
    client.println("});");
    client.println("$(\"#date\").click(function() {$(\"#msg_txt\").prop(\"disabled\", true);});");
    client.println("$(\"#temp\").click(function() {$(\"#msg_txt\").prop(\"disabled\", true);});");
    client.println("$(\"#dt\").click(function() {$(\"#msg_txt\").prop(\"disabled\", true);});");
    client.println("$(\"#msg\").click(function() {$(\"#msg_txt\").prop(\"disabled\", false);});");
    client.println("$('#backlight_schedule').change(function() {       $(\"#boff\").prop(\"disabled\",!$(this).is(':checked'));       $(\"#bon\").prop(\"disabled\",!$(this).is(':checked')); });");
    client.println("});");
    client.println("</script>");
    client.println("</head>");
    client.println("<body>");
    client.println("<nav id=\"nav\" class=\"navbar navbar-default\" style=\"padding-left:50px; padding-right:50px;\">");
    client.println("<div class=\"navbar-header\">");
    client.println("<h1 class=\"banner\"><a href=\"/\">Tram Times</a> - <small>by Steve</small></h1>");
    client.println("</div>");
    client.println("<ul class=\"nav navbar-nav navbar-right\">");
    client.println("<li><a href=\"https://steve.fi/Hardware/\">Steve's Projects</a></li>");
    client.println("</ul>");
    client.println("</nav>");
    client.println("<div class=\"container\">");
    client.println("<h1 class=\"underline\">Tram Times</h1>");
    client.println("<p>&nbsp;</p>");
    client.println("<blockquote>");
    client.println("<table class=\"table table-striped table-hover table-condensed table-bordered\">");


    // Now show the calculated tram-times

    // For each row.
    for (int i = 0; i < NUM_ROWS; i++)
    {
        client.print("<tr><td><code>");

        int len = strlen(screen[i]);

        // Print the text in this line.
        for (int j = 0; j < len; j++)
        {
            //
            // But ensure that we change 0xDF
            // into the degree symbol.
            //
            // Without this we get a "?" in the
            // HTML-output.
            //
            if (screen[i][j] == 0xDF)
                client.print("&deg;");
            else
                client.print(screen[i][j]);
        }

        client.print("</code></td></tr>");
    }

    client.println("</table>");
    client.println("</blockquote>");
    client.println("<h2 class=\"underline\">Configuration</h2>");
    client.println("<p>&nbsp;</p>");

    // Tab navigation
    client.println("<ul class=\"nav nav-tabs\">");
    client.println("<li class=\"active\"><a data-toggle=\"tab\" href=\"#backlight\">Backlight</a></li>");
    client.println("<li><a data-toggle=\"tab\" href=\"#display\">Display</a></li>");
    client.println("<li><a data-toggle=\"tab\" href=\"#config\">Configuration</a></li>");
    client.println("<li><a data-toggle=\"tab\" href=\"#debug\">Debug</a></li>");
    client.println("</ul>");


    // Tab content
    client.println("<div class=\"tab-content\">");

    // Tab 1 - Backlight
    client.println("<div id=\"backlight\" class=\"tab-pane fade in active\">");
    client.println("<blockquote>");
    client.println("<p>&nbsp;</p>");
    client.println("<table class=\"table table-striped table-hover table-condensed table-bordered\">");
    client.println("<tr><td><b>Backlight</b></td><td>");

    // Showing the state.
    if (backlight)
        client.println("<p>On, <a href=\"/?backlight=off\">turn off</a>.</p>");
    else
        client.println("<p>Off, <a href=\"/?backlight=on\">turn on</a>.</p>");

    client.println("</td></tr>");

    //
    // Is there a schedule setup?
    //
    bool scheduled = true;

    if ((backlight_on == -1) && (backlight_off == -1))
        scheduled = false;

    // Backlight schedule.
    client.println("<tr><td><b>Backlight Schedule</b></td><td>");
    client.println("<form action=\"/\" METHOD=\"GET\">");
    client.println("<input type=\"hidden\" name=\"schedule\" value=\"yes\">");
    client.printf("<p><input type=\"checkbox\" id=\"backlight_schedule\" name=\"backlight_schedule\"%s>Enable scheduling</p>", scheduled ? " checked=\"checked\"" : "");
    client.print("<p>Turn on at");

    output_select(client, "bon", scheduled, backlight_on);

    client.println(" turn off at ");

    output_select(client, "boff", scheduled, backlight_off);

    client.println("<input type=\"submit\" value=\"Update\"></p></form></td></tr>");

    client.println("</table>");
    client.println("</blockquote>");
    client.println("</div>");

    // Tab 2 - Display
    client.println("<div id=\"display\" class=\"tab-pane fade\">");
    client.println("<p>&nbsp;</p>");
    client.println("<blockquote>");
    client.println("<form action=\"/\" method=\"GET\">");

    client.printf("<p><input name=\"mode\" id=\"date\" value=\"date\" type=\"radio\" %s>Show date</p>", g_state == DATE ? " checked=\"checked\"" : "");
    client.printf("<p><input name=\"mode\" id=\"temp\" value=\"temp\" type=\"radio\"%s>Show temperature</p>", g_state == TEMPERATURE ? " checked=\"checked\"" : "");
    client.printf("<p><input name=\"mode\" id=\"dt\" value=\"dt\" type=\"radio\"%s>Alternate date &amp; temperature</p>", g_state == DATE_OR_TEMP ? " checked=\"checked\"" : "");
    client.printf("<p><input name=\"mode\" id=\"msg\" value=\"msg\" type=\"radio\"%s>Show a message - <input type=\"text\" id=\"msg_txt\" name=\"msg_txt\" value=\"%s\" %s></p>",
                  g_state == MESSAGE ? " checked=\"checked\"" : "", g_msg, g_state != MESSAGE ? "disabled" : "");
    client.println("<p><input type=\"submit\" value=\"Update\"></p>");
    client.println("</form>");
    client.println("</blockquote>");
    client.println("</div>");

    // Tab 3 - Configuration
    client.println("<div id=\"config\" class=\"tab-pane fade\">");
    client.println("<p>&nbsp;</p>");
    client.println("<blockquote>");
    client.println("<table class=\"table table-striped table-hover table-condensed table-bordered\">");
    client.println("<tr><td><b>Timezone</b></td>");
    client.print("  <td><form action=\"/\" method=\"GET\"><input type=\"text\" name=\"tz\" value=\"");

    if (time_zone_offset > 0)
        client.print("+");

    if (time_zone_offset < 0)
        client.print("-");

    client.print(time_zone_offset);

    client.println("\"><input type=\"submit\" value=\"Update\"></form></td></tr>");
    client.println("<tr><td><b>Tram Stop</b></td>");
    client.printf("<td><form action=\"/\" method=\"GET\"><input type=\"text\" name=\"stop\" value=\"%s\">", tram_stop);
    client.print("<input type=\"submit\" value=\"Update\"></form>");
    client.printf("<a href=\"https://www.reittiopas.fi/pysakit/HSL:%s\">View on map</a>", tram_stop);
    client.println("</td></tr>");
    client.println("<tr><td><b>Tram API</b></td>");
    client.printf("<td><form action=\"/\" method=\"GET\"><input type=\"text\" name=\"api\" size=\"75\" value=\"%s\">", api_end_point);
    client.println("<input type=\"submit\" value=\"Update\"></form></td></tr>");
    client.println("<tr><td><b>Temperature API</b></td>");
    client.printf("<td><form action=\"/\" method=\"GET\"><input type=\"text\" name=\"temp\" size=\"75\" value=\"%s\">", temp_end_point);
    client.println("<input type=\"submit\" value=\"Update\"></form></td></tr>");
    client.println("</table>");
    client.println("</blockquote>");
    client.println("</div>");

    // Tab 4 - Debug
    client.println("<div id=\"debug\" class=\"tab-pane fade\">");
    client.println("<p>&nbsp;</p>");
    client.println("<blockquote>");
#ifdef DEBUG
    client.print("<p>Debugging logs:</p><blockquote>");
    client.println("<table class=\"table table-striped table-hover table-condensed table-bordered\">");

    for (int i = 0; i < DEBUG_MAX; i++)
    {
        if (debug_logs[i] != "")
        {
            client.print("<tr><td>");
            client.print(i);
            client.print("</td><td>");
            client.print(debug_logs[i]);
            client.print("</td></tr>");
        }
    }

    client.println("</table></blockquote>");
#endif
    client.println("<p>Uptime:</p><blockquote>");


    long currentmillis = millis();
    long days = 0;
    long hours = 0;
    long mins = 0;
    long secs = 0;
    secs = currentmillis / 1000;
    mins = secs / 60;
    hours = mins / 60;
    days = hours / 24;
    secs = secs - (mins * 60);
    mins = mins - (hours * 60);
    hours = hours - (days * 24);

    client.printf("<p>%d day%s, %d hours, %d minutes, %d seconds.</p>", days, days == 1 ? "" : "s", hours, mins, secs);
    client.print("</blockquote>");


    client.println("</blockquote>");
    client.println("</div>");
    client.println("</div>");
    client.println("</div>");
    client.println("</body>");
    client.println("</html>");

}


//
// Open the given file for writing, and write out the specified data.
//
void write_file(const char *path, const char *data)
{
    File f = SPIFFS.open(path, "w");

    if (f)
    {
        DEBUG_LOG("Writing data '%s' to file '%s'\n", data, path);
        f.println(data);
        f.close();
    }
}

//
// Read a single line from a given file, and return it.
//
// This function explicitly filters out `\r\n`, and only
// reads the first line of the text.
//
String read_file(const char *path)
{
    String line;

    File f = SPIFFS.open(path, "r");

    if (f)
    {
        line = f.readStringUntil('\n');
        f.close();
    }


    String result;

    for (int i = 0; i < line.length() ; i++)
    {
        if ((line[i] != '\n') &&
                (line[i] != '\r'))
            result += line[i];
    }

    DEBUG_LOG("Read '%s' from file '%s'\n", result.c_str(), path);
    return (result);
}

//
// Draw a single line of text on the given row of our
// LCD.
//
// This is used to ensure that each line drawn is always
// padded with spaces.  This is required to avoid
// display artifacts if you draw:
//
//    THIS IS A LONG LINE
// then
//    HELLO
//
// (Which would show "HELLOIS A LONG LINE".)
//
void draw_line(int row, const char *txt)
{
    lcd.setCursor(0, row);
    lcd.print(txt);

    int extra = NUM_COLS - strlen(txt);

    while (extra > 0)
    {
        lcd.print(" ");
        extra--;
    }

}

//
// Process an incoming HTTP-request.
//
// Here we look for GET requests that are updating our configuration-options
// and if they're found we handle them.
//
// If we handled something we might issue a redirection back to the server
// root - otherwise we return the same HTML every time.  There's no AJAX
// or other dynamic action happening.
//
void processHTTPRequest(WiFiClient client)
{
    // Wait until the client sends some data
    while (client.connected() && !client.available())
        delay(1);

    // Read the first line of the request
    String request = client.readStringUntil('\r');
    client.flush();

    //
    // Find the URL we were requested
    //
    // We'll have something like "GET XXXXX HTTP/XX"
    // so we split at the space and send the "XXX HTTP/XX" value
    //
    request = request.substring(request.indexOf(" ")  + 1);

    //
    // Now we'll want to peel off any HTTP-parameters that might
    // be present, via our utility-helper.
    //
    URL url(request.c_str());

    //
    // Does the user want to change the backlight?
    //
    char *blight = url.param("backlight");

    if (blight != NULL)
    {

        // Turn on?
        if (strcmp(blight, "on") == 0)
        {
            backlight = true;
            lcd.setBacklight(true);
        }

        // Turn off?
        if (strcmp(blight, "off") == 0)
        {
            backlight = false;
            lcd.setBacklight(false);
        }

        // Redirect to the server-root
        redirectIndex(client);
        return;
    }

    //
    // Does the user want to change the tram-stop?
    //
    char *stop = url.param("stop");

    if (stop != NULL)
    {
        // Update the tram ID
        strncpy(tram_stop, stop, sizeof(tram_stop) - 1);

        // Record the data in a file.
        write_file("/tram.stop", tram_stop);

        // So we've changed the tram ID we should refresh the date.
        //
        // We force this to happen by updating our screen
        //
        for (int i = 1; i  < NUM_ROWS; i++)
        {
            screen[i][0] = '\0';
        }


        // Redirect to the server-root
        redirectIndex(client);
        return;
    }

    //
    // Does the user want to change the display-mode?
    //
    char *mode = url.param("mode");

    if (mode != NULL)
    {
        // Save the updated value to flash.
        write_file("/display.mode", mode);

        // Now make it take effect.
        set_display_mode(mode);

        // We might have a message to go along with the mode
        char *msg = url.param("msg_txt");

        if (msg != NULL)
        {
            // Clear the old message.
            memset(g_msg, '\0', sizeof(g_msg));

            // If the message does not contain "#" then save it.
            if (strchr(msg, '#') == NULL)
            {
                strncpy(g_msg, msg, sizeof(g_msg) - 1);

                // Save the message
                write_file("/text.msg", msg);
            }
        }


        // Redirect to the server-root
        redirectIndex(client);
        return;

    }


    //
    // Is the user setting up a schedule?
    //
    char *schedule = url.param("schedule");

    if (schedule != NULL)
    {
        //
        // OK there might be three parameters:
        //
        // bon                -> Time the backlight comes on.
        // boff               -> Time the backlight goes off.
        // backlight_schedule -> 1 if enabled.
        //
        char *s = url.param("backlight_schedule");
        char *on = url.param("bon");
        char *off = url.param("boff");

        // If the checkbox is ticked
        if ((s != NULL) && (strlen(s) > 0))
        {
            // if the on-time is setup
            if (on != NULL)
            {
                // record it, and make it live too.
                write_file("/b.on", on);
                backlight_on = atoi(on);
            }

            // if the off-time is setup
            if (off != NULL)
            {
                // record it, and make it live too.
                write_file("/b.off", off);
                backlight_off = atoi(off);
            }
        }
        else
        {
            // Prevent the schedule by writing "-1" to the on/off times
            write_file("/b.on", "-1\n");
            write_file("/b.off", "-1\n");

            // disable the schedule, if it was previously set.
            backlight_on = -1;
            backlight_off = -1;
        }

        redirectIndex(client);
        return;

    }

    //
    // Does the user want to change the tram-API end-point?
    //
    char *api = url.param("api");

    if (api != NULL)
    {
        // Update the API end-point
        strncpy(api_end_point, api, sizeof(api_end_point) - 1);

        // Record the data in a file.
        write_file("/tram.api", api_end_point);

        // So we've changed the tram ID we should refresh the date.
        fetch_tram_times();

        // Redirect to the server-root
        redirectIndex(client);
        return;
    }

    //
    // Does the user want to change the temperature API end-point?
    //
    char *temp = url.param("temp");

    if (temp != NULL)
    {
        // Update the API end-point
        strncpy(temp_end_point, temp, sizeof(temp_end_point) - 1);

        // Record the data in a file.
        write_file("/temp.api", temp_end_point);

        // So we've changed the temperature ID we should refresh the date.
        fetch_temperature();

        // Redirect to the server-root
        redirectIndex(client);
        return;
    }

    //
    // Does the user want to change the time-zone?
    //
    char *tz = url.param("tz");

    if (tz != NULL)
    {
        // Record the data into a file.
        write_file("/time.zone", tz);

        // Change the timezone now
        time_zone_offset = atoi(tz);

        // Update the offset.
        timeClient.setTimeOffset(time_zone_offset * (60 * 60));
        timeClient.forceUpdate();

        // Redirect to the server-root
        redirectIndex(client);
        return;
    }


    //
    // At this point we've either received zero URL-paramters
    // or we've only received ones we didn't recognize.
    //
    // Either way return a simple response.
    //
    serveHTML(client);

}
