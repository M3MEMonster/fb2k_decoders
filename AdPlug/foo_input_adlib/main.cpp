#include "stdafx.h"
#include "adplug/version.h"

DECLARE_COMPONENT_VERSION(
    "AdPlug OPL Decoder",
    "1.0",
    "AdPlug " ADPLUG_VERSION " based OPL2/OPL3 music decoder.\n"
    "Supports 50+ AdLib/OPL music formats.\n\n"
    "Based on AdPlug library by Simon Peter, et al.\n"
    "AdPlug is licensed under LGPL 2.1.\n\n"
    "OPL emulation: DOSBox OPL3 emulator."
);

VALIDATE_COMPONENT_FILENAME("foo_input_adlib.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
