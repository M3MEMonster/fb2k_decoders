#include <foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "Psycle Decoder",
    "1.12.2",
    "Psycle Modular Music Creation Studio decoder for foobar2000.\n"
    "Based on Psycle 1.12.2 engine (rePlayer port).\n"
    "Original Psycle: Copyright(c) 2000-2014 Psycledelics\n"
    "https://psycle.sourceforge.net"
);

VALIDATE_COMPONENT_FILENAME("foo_psycle.dll");

DECLARE_FILE_TYPE("Psycle songs", "*.PSY");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
