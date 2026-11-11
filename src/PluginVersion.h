#pragma once

// Release tools and the native UI/factory read the same product identity.
#define RVC_PLUGIN_VERSION "1.1.9"
#define RVC_PLUGIN_BUILD "20261111.2"
#define RVC_PLUGIN_VENDOR "Taketo"
#define RVC_PLUGIN_PRODUCT "RVC Feature Injector"

#define RVC_PLUGIN_WIDEN_IMPL(value) L##value
#define RVC_PLUGIN_WIDEN(value) RVC_PLUGIN_WIDEN_IMPL(value)

namespace rvc_vst::plugin_info {
inline constexpr char kVersion[] = RVC_PLUGIN_VERSION;
inline constexpr wchar_t kVersionWide[] = RVC_PLUGIN_WIDEN(RVC_PLUGIN_VERSION);
inline constexpr char kBuild[] = RVC_PLUGIN_BUILD;
inline constexpr wchar_t kBuildWide[] = RVC_PLUGIN_WIDEN(RVC_PLUGIN_BUILD);
inline constexpr char kVendor[] = RVC_PLUGIN_VENDOR;
inline constexpr wchar_t kVendorWide[] = RVC_PLUGIN_WIDEN(RVC_PLUGIN_VENDOR);
inline constexpr char kProductName[] = RVC_PLUGIN_PRODUCT;
inline constexpr wchar_t kProductNameWide[] = RVC_PLUGIN_WIDEN(RVC_PLUGIN_PRODUCT);
inline constexpr wchar_t kFormatWide[] = L"VST3 / Windows x64";
}
