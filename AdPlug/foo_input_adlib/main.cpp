#include "stdafx.h"
#include "adplug/version.h"

DECLARE_COMPONENT_VERSION(
    "AdPlug OPL Decoder",
    "1.1",
    "AdPlug " ADPLUG_VERSION " based OPL2/OPL3 music decoder.\n"
    "Supports 50+ AdLib/OPL music formats.\n\n"
    "Based on AdPlug library by Simon Peter, et al.\n"
    "AdPlug is licensed under LGPL 2.1.\n\n"
    "OPL cores: Harekiet's (DOSBox), Ken Silverman's, Jarek Burczynski's (FMOPL), Nuked OPL3.\n"
    "Configurable via Preferences \xe2\x80\x93 Playback \xe2\x80\x93 Decoding \xe2\x80\x93 AdLib."
);

VALIDATE_COMPONENT_FILENAME("foo_input_adlib.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
