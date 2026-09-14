/**
 * @file net.h
 * @brief Network bring-up for the Tab5: ESP32-C6 co-processor link, WiFi station, SNTP.
 *
 * The Tab5's radio is on the ESP32-C6, reached over SDIO through esp_hosted. Once the link is up
 * the regular esp_wifi API applies transparently. WiFi credentials come from secrets.h.
 */
#pragma once

namespace net {

/** Power the C6, bring up the hosted link, update its firmware if needed, and start WiFi. */
void start();

/** Block until the station has an IP address. */
void wait_connected();

/** True while the station holds an IP address. */
bool connected();

/** Dotted IP address of the station, or "" when not connected. */
const char *ip();

}
