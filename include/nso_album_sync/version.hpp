#pragma once

// CMake defines these from project(VERSION) and the NSO_NXAPI_AUTH_CLIENT_ID /
// NSO_ZNCA_CLIENT_VERSION cache variables. The fallbacks only keep tools that
// compile a single file without the build definitions working, and match the
// development defaults in CMakeLists.txt.
#ifndef NSO_APP_VERSION
#define NSO_APP_VERSION "0.0.0"
#endif

#ifndef NSO_NXAPI_AUTH_CLIENT_ID
#define NSO_NXAPI_AUTH_CLIENT_ID "9F_ul8eygxiLn9N0uCaPeA"
#endif

#ifndef NSO_ZNCA_CLIENT_VERSION
#define NSO_ZNCA_CLIENT_VERSION "d8fAZDPzwimzQ7c6"
#endif

#define NSO_PROJECT_URL "https://github.com/minimalhandle/Switch-Album-Sync"
