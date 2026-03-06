#pragma once

#include <helpers/foobar2000+atl.h>
#include <helpers/atl-misc.h>
#include <SDK/coreDarkMode.h>

// Target foobar2000 v2.0+ (override SDK default of 80)
#undef FOOBAR2000_TARGET_VERSION
#define FOOBAR2000_TARGET_VERSION 81

#include <libPPUI/CListControlOwnerData.h>
#include <libPPUI/CListControl-Cells.h>
#include <libPPUI/CListControlSimple.h>
#include <helpers/CListControlFb2kColors.h>

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <sstream>
