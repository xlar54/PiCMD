#ifndef WIFI_DATA_LINK_H
#define WIFI_DATA_LINK_H
#include "ntpclient.h"
bool WifiDataLink_GetMac(u8 mac[6]);
NetFrameIO WifiDataLink_GetFrameIO();
#endif
