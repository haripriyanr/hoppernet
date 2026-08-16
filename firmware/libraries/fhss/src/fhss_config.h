#ifndef FHSS_CONFIG_H
#define FHSS_CONFIG_H

// ========================================================
// HopperNet (MedRelay) Network & Cloud Configuration
// ========================================================

#define HOPPERNET_WIFI_SSID     "hoppernet"
#define HOPPERNET_WIFI_PASS     "password"

#define HOPPERNET_SUPABASE_URL  "https://pbebctpsswuesmgkmecn.supabase.co"
#define HOPPERNET_SUPABASE_KEY  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InBiZWJjdHBzc3d1ZXNtZ2ttZWNuIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY5MDE0NzksImV4cCI6MjEwMjQ3NzQ3OX0.NZT-FFmEjprHTlcYeAYCkI5yCY5wbhW7iP9H0_AlxWk"

// Hardware Pinout for all Mesh ESP32 Nodes (A, B, C)
#define RF_CE_PIN               4
#define RF_CSN_PIN              5
#define RF_SCK_PIN              18
#define RF_MISO_PIN             19
#define RF_MOSI_PIN             23

// Jammer Dedicated Pins (ESP32 #4)
#define JAMMER_CE_PIN           25
#define JAMMER_CSN_PIN          26

#endif // FHSS_CONFIG_H
