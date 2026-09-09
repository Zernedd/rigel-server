// pch.h — precompiled header for HalcyonA2.
//
// Order matters: Windows first (WIN32_LEAN_AND_MEAN keeps winsock1 out of the way so
// <winhttp.h> / <iphlpapi.h> in dllmain.cpp include cleanly), then the Dumper-7 SDK.
#pragma once

// Also set project-wide, so guard them: a plain #define here is a C4005 redefinition.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <cstdint>
#include <cstring>

// Dumper-7 output for the build selected by $(GameBuild) (HALCYON_GAME_BUILD): the include
// path points at gamesdk\<build>\. Generate a new one with tools\Dump-A2.ps1 -GameBuild N.
// The SDK is build specific; so is every offset in GameOffsets.h.
#if __has_include("SDK.hpp")
#include "SDK.hpp"
#else
#error "SDK.hpp not found for this GameBuild. Run tools\Dump-A2.ps1 (it installs into gamesdk\<build>\). See README.md."
#endif

#include "GameOffsets.h"
