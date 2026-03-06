#include <foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "Quartet Decoder",
    "1.5.0",
    "Quartet (Atari ST) music decoder for foobar2000.\n"
    "Based on zingzong 1.5.0.294 by Benjamin Gerard AKA Ben/OVR.\n"
    "Supports .4v and .qts Quartet music files.\n"
    "https://sourceforge.net/projects/sc68/"
);

VALIDATE_COMPONENT_FILENAME("foo_quartet.dll");

DECLARE_FILE_TYPE("Quartet music files", "*.4V;*.QTS");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
