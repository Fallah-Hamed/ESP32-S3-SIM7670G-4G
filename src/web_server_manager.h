#pragma once

#include <Arduino.h>

#define HTTP_PORT 80
#define WS_PORT   81

void initWebServer();
void handleClients();
void webServerSendTXT(uint8_t clientNum, const char* msg);
