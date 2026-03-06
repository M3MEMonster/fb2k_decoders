#include <foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "Nostalgia",
    "1.0.0",
    "Multi-tracker decoder plugin for foobar2000.\n"
    "Includes:\n"
    "  GoatTracker 2.77 - C64 SID tracker by Lasse Oorni\n"
    "  ProTrekkr 2.8.2 - Modular tracker by Franck Charlet\n"
    "  MegaTracker Player 4.3.1 - Apple IIgs formats by Ian Schmidt\n"
    "  Skale Tracker 0.81 - Windows tracker by Ruben Ramos Salvador\n"
    "  Klystrack - Chiptune tracker format (.kt)\n"
    "  MONOTONE - SoLoud monotone tracker format (.mon)\n"
    "  FAC SoundTracker - OPL2 module format (.mus)"
);

VALIDATE_COMPONENT_FILENAME("foo_nostalgia.dll");

DECLARE_FILE_TYPE("GoatTracker songs", "*.SNG");
DECLARE_FILE_TYPE("ProTrekkr modules", "*.PTK");
DECLARE_FILE_TYPE("Skale Tracker modules", "*.SKM");
DECLARE_FILE_TYPE("Klystrack modules", "*.KT");
DECLARE_FILE_TYPE("MONOTONE modules", "*.MON");
DECLARE_FILE_TYPE("FAC SoundTracker modules", "*.MUS");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
