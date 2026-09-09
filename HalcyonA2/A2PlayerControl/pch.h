// pch.h - precompiled header for A2PlayerControl (UE4SS native mod).
//
// Same ordering rule as HalcyonA2's pch: Windows first, then the Dumper-7 SDK for the
// build selected by $(GameBuild).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <share.h>
#include <string>

#if __has_include("SDK.hpp")
#include "SDK.hpp"
#else
#error "SDK.hpp not found for this GameBuild. The include path must point at HalcyonA2\\gamesdk\\<build>\\."
#endif
