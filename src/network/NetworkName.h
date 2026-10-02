#pragma once

// How the device presents itself on the network (mDNS <name>.local, DHCP, AP SSID).
// The UDP discovery reply keeps its "crosspoint (on ...)" prefix for client compatibility.
constexpr const char* NET_HOSTNAME = "crossdink";
constexpr const char* NET_AP_SSID = "CrossDink-Reader";
