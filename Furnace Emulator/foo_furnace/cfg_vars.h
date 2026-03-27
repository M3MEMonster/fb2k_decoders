#pragma once
#include <SDK/cfg_var.h>

namespace furnace_cfg {

// {E1A2B3C4-D5E6-4F70-8192-A3B4C5D6E7F8}
static constexpr GUID guid_enable_fur = { 0xe1a2b3c4, 0xd5e6, 0x4f70, { 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 } };
// {F2B3C4D5-E6F7-4081-92A3-B4C5D6E7F809}
static constexpr GUID guid_enable_dmf = { 0xf2b3c4d5, 0xe6f7, 0x4081, { 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09 } };
// {03C4D5E6-F708-4192-A3B4-C5D6E7F80A1B}
static constexpr GUID guid_enable_ftm = { 0x03c4d5e6, 0xf708, 0x4192, { 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x0a, 0x1b } };
// {14D5E6F7-0819-42A3-B4C5-D6E7F80A1B2C}
static constexpr GUID guid_enable_dnm = { 0x14d5e6f7, 0x0819, 0x42a3, { 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x0a, 0x1b, 0x2c } };
// {25E6F708-192A-43B4-C5D6-E7F80A1B2C3D}
static constexpr GUID guid_enable_0cc = { 0x25e6f708, 0x192a, 0x43b4, { 0xc5, 0xd6, 0xe7, 0xf8, 0x0a, 0x1b, 0x2c, 0x3d } };
// {36F70819-2A3B-44C5-D6E7-F80A1B2C3D4E}
static constexpr GUID guid_enable_eft = { 0x36f70819, 0x2a3b, 0x44c5, { 0xd6, 0xe7, 0xf8, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e } };
// {4708192A-3B4C-45D6-E7F8-0A1B2C3D4E5F}
static constexpr GUID guid_enable_tfm = { 0x4708192a, 0x3b4c, 0x45d6, { 0xe7, 0xf8, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f } };
// {5819203A-4B5C-46E7-F80A-1B2C3D4E5F60}
static constexpr GUID guid_enable_tfe = { 0x5819203a, 0x4b5c, 0x46e7, { 0xf8, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60 } };
// {692A3B4C-5D6E-47F8-0A1B-2C3D4E5F6071}
static constexpr GUID guid_default_duration = { 0x692a3b4c, 0x5d6e, 0x47f8, { 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71 } };

inline cfg_bool cfg_fur_enabled(guid_enable_fur, true);
inline cfg_bool cfg_dmf_enabled(guid_enable_dmf, true);
inline cfg_bool cfg_ftm_enabled(guid_enable_ftm, true);
inline cfg_bool cfg_dnm_enabled(guid_enable_dnm, true);
inline cfg_bool cfg_0cc_enabled(guid_enable_0cc, true);
inline cfg_bool cfg_eft_enabled(guid_enable_eft, true);
inline cfg_bool cfg_tfm_enabled(guid_enable_tfm, true);
inline cfg_bool cfg_tfe_enabled(guid_enable_tfe, true);
inline cfg_int  cfg_default_duration_sec(guid_default_duration, 180);

} // namespace furnace_cfg
