#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
    "Ayumi Decoder",
    "1.0",
    "Ayumi AY/YM chip music decoder for foobar2000\n"
    "Based on Ayumi by Peter Sovietov\n"
    "webAyumi by Juergen Wothke"
);

VALIDATE_COMPONENT_FILENAME("foo_ayumi.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
