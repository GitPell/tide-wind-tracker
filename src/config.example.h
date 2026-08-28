#pragma once

// Copy to config.h and fill in. Keep real credentials out of version control.

#define WIFI_SSID           "your-ssid"
#define WIFI_PASS           "your-password"

// NOAA CO-OPS station. Find yours at:
//   https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations.json
#define NOAA_STATION        "9414290"
#define STATION_LABEL       "GOLDEN GATE"

// Coordinates for the Open-Meteo wind forecast.
#define SITE_LAT            37.8063
#define SITE_LON           -122.4659

// Local timezone, POSIX TZ string. This one is US Pacific with DST.
#define TZ_STRING           "PST8PDT,M3.2.0,M11.1.0"

// Minutes between refreshes. Below ~15 the panel refresh itself starts to
// dominate the duty cycle and battery life falls off a cliff.
#define UPDATE_MINUTES      30

// Force a full clear-and-redraw every N cycles to suppress ghosting.
#define FULL_REFRESH_EVERY  12

#define WIFI_TIMEOUT_MS     20000
#define HTTP_TIMEOUT_MS     15000
