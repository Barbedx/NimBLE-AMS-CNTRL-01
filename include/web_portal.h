#pragma once

// PsychicHttp dashboard: pure presentation/control. Reads state from the
// service modules (AMS/ANCS/CTS via Bluetooth) and Wi-Fi state from WiFiManager,
// and exposes media controls + a Wi-Fi config form. Knows nothing about how the
// network is brought up. The server runs in its own task, so there is no Handle().
namespace WebPortal {
    void Begin();
}
