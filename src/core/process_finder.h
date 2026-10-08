#pragma once

#include <cstdint>
#include <string>

// Finds a process by executable name without depending on a platform process
// enumeration API.  The Windows implementation uses Toolhelp; Linux uses
// /proc.  Keeping discovery separate makes the matching behavior consistent
// for native Linux processes and Windows builds.
uint32_t FindProcessIdByName(const std::wstring& processName);
