#include <foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "Furnace Emulator",
    "1.0.0",
    "Decoder for Furnace Tracker and compatible formats.\n"
    "Based on Furnace " "0.6.8.3" "\n"
    "Copyright (c) 2021-2025 tildearrow and contributors\n\n"
    "Supported formats:\n"
    "  .fur  - Furnace Tracker native\n"
    "  .dmf  - DefleMask\n"
    "  .ftm  - FamiTracker\n"
    "  .dnm  - DefleMask-converted FamiTracker\n"
    "  .0cc  - 0CC-FamiTracker\n"
    "  .eft  - Enhanced FamiTracker\n"
    "  .tfm  - TurboSound FM\n"
    "  .tfe  - TurboSound FM Extended"
);

VALIDATE_COMPONENT_FILENAME("foo_furnace.dll");

DECLARE_FILE_TYPE("Furnace Tracker modules", "*.FUR");
DECLARE_FILE_TYPE("DefleMask modules", "*.DMF");
DECLARE_FILE_TYPE("FamiTracker modules", "*.FTM;*.DNM;*.0CC;*.EFT");
DECLARE_FILE_TYPE("TurboSound FM modules", "*.TFM;*.TFE");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
