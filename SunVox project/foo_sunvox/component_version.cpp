#include <foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "SunVox Decoder",
    "2.1.4",
    "SunVox modular synthesizer music decoder for foobar2000.\n"
    "Based on SunVox engine 2.1.4 by Alexander Zolotov (NightRadio).\n"
    "Supports .sunvox project files.\n"
    "https://warmplace.ru/soft/sunvox/"
);

VALIDATE_COMPONENT_FILENAME("foo_sunvox.dll");

DECLARE_FILE_TYPE("SunVox music files", "*.SUNVOX");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
