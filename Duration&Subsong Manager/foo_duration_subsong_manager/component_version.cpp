#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
    "Duration & Subsong Manager",
    "1.0",
    "Manages per-track custom durations and subsong visibility.\n\n"
    "Duration Manager: maintain a database of custom durations that override\n"
    "the actual track length for both display and playback.\n\n"
    "Subsong Manager: control which subsongs from multi-subsong files\n"
    "appear in playlists, with persistent exclusion across library rescans."
);

VALIDATE_COMPONENT_FILENAME("foo_duration_subsong_manager.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
