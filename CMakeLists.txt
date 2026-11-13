cmake_minimum_required(VERSION 3.20)

project(
    SwitchAlbumSync
    VERSION 1.0.0
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Switch Album Sync currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(
    NSO_STATIC_RUNTIME
    "Prefer static language/TLS runtimes for compact standalone releases"
    OFF
)
option(NSO_ENABLE_DISCORD "Build Discord Rich Presence using the Discord Social SDK" ON)
set(
    NSO_DISCORD_SOCIAL_SDK_VERSION
    "1.10.18687"
    CACHE STRING
    "Discord Social SDK version used for Rich Presence"
)
set(
    NSO_DISCORD_SOCIAL_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/discord_social_sdk"
    CACHE PATH
    "Extracted Discord Social SDK root"
)
# nxapi-znca-api's public API terms require a separate client for development.
# Builds default to the development client; build-release.ps1 passes the release
# client. If you build your own copy, register your own client at
# https://nxapi-auth.fancy.org.uk/oauth/clients and pass its ID here.
set(
    NSO_NXAPI_AUTH_CLIENT_ID
    "9F_ul8eygxiLn9N0uCaPeA"
    CACHE STRING
    "nxapi-auth OAuth client ID (Switch Album Sync development client by default)"
)
# Sent as X-znca-Client-Version; must be fixed, never set at runtime. nxapi now
# rejects 3.0.x values for current Nintendo Switch Online versions
# (incompatible_client), and nxapi-auth doesn't yet issue per-client values.
# Until it does, builds use the compatibility value nxapi's own client
# publishes (ZNCA_API_COMPATIBILITY_VERSION in nxapi src/api/coral.ts).
set(
    NSO_ZNCA_CLIENT_VERSION
    "d8fAZDPzwimzQ7c6"
    CACHE STRING
    "Fixed X-znca-Client-Version value for nxapi-znca-api"
)
if(NSO_NXAPI_AUTH_CLIENT_ID STREQUAL "" OR NSO_ZNCA_CLIENT_VERSION STREQUAL "")
    message(FATAL_ERROR "NSO_NXAPI_AUTH_CLIENT_ID and NSO_ZNCA_CLIENT_VERSION must not be empty")
endif()

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
    add_link_options(
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()

if(NSO_STATIC_RUNTIME)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

set(NSO_THIRD_PARTY_NOTICES "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt")
if(NOT EXISTS "${NSO_THIRD_PARTY_NOTICES}")
    message(FATAL_ERROR "THIRD-PARTY-NOTICES.txt is required for release packaging")
endif()
set(NSO_THIRD_PARTY_NOTICES_RESOURCE_ID 202)

if(NSO_ENABLE_DISCORD)
    # Rich Presence uses Discord's current Social SDK only. CI checks out the
    # private SDK cache directly at NSO_DISCORD_SOCIAL_SDK_ROOT. Local builds can
    # point this variable at an official SDK extracted from the Developer Portal.
    # The desktop Rich Presence path does not use Discord OAuth, bot tokens,
    # client secrets, or Client::Connect().
    if(NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/discordpp.h" OR
       NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/cdiscord.h")
        message(FATAL_ERROR
            "Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION} was not found at "
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}. Extract the official standalone C++ SDK "
            "there, set -DNSO_DISCORD_SOCIAL_SDK_ROOT=<path>, or configure with "
            "-DNSO_ENABLE_DISCORD=OFF."
        )
    endif()

    set(NSO_DISCORD_SDK_INCLUDE_DIR "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include")
    set(NSO_DISCORD_SDK_RESOURCE_ID 201)

    if(NOT MSVC)
        message(FATAL_ERROR "Discord Social SDK Windows builds currently require MSVC")
    endif()
    set(NSO_DISCORD_SDK_LINK_LIBRARY
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/discord_partner_sdk.lib")
    set(NSO_DISCORD_SDK_RUNTIME
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/bin/release/discord_partner_sdk.dll")

    if(NOT EXISTS "${NSO_DISCORD_SDK_LINK_LIBRARY}" OR
       NOT EXISTS "${NSO_DISCORD_SDK_RUNTIME}")
        message(FATAL_ERROR
            "Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION} is missing the "
            "release runtime/link library required for this platform under "
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}."
        )
    endif()
endif()

set(NSO_SOURCES
    src/main.cpp
    src/app.cpp
    src/auth_callback.cpp
    src/config.cpp
    src/coral.cpp
    src/discord.cpp
    src/game_services.cpp
    src/json.cpp
    src/local_data.cpp
    src/nintendo_auth.cpp
    src/nxapi.cpp
    src/rpc_enricher.cpp
    src/splatnet.cpp
    src/sync.cpp
    src/util.cpp
    src/zeldanotes.cpp
    src/zeldanotes_regions.cpp
)

set(NSO_HEADERS
    include/nso_album_sync/appearance.hpp
    include/nso_album_sync/app.hpp
    include/nso_album_sync/auto_sync_policy.hpp
    include/nso_album_sync/backup.hpp
    include/nso_album_sync/auth_callback.hpp
    include/nso_album_sync/config.hpp
    include/nso_album_sync/coral.hpp
    include/nso_album_sync/discord.hpp
    include/nso_album_sync/game_aliases.hpp
    include/nso_album_sync/game_services.hpp
    include/nso_album_sync/http.hpp
    include/nso_album_sync/json.hpp
    include/nso_album_sync/nintendo_auth.hpp
    include/nso_album_sync/nxapi.hpp
    include/nso_album_sync/platform.hpp
    include/nso_album_sync/rpc.hpp
    include/nso_album_sync/rpc_enricher.hpp
    include/nso_album_sync/rpc_settings.hpp
    include/nso_album_sync/secure_store.hpp
    include/nso_album_sync/splatnet.hpp
    include/nso_album_sync/sync.hpp
    include/nso_album_sync/util.hpp
    include/nso_album_sync/version.hpp
    include/nso_album_sync/zeldanotes.hpp
    include/nso_album_sync/zeldanotes_regions.hpp
)

enable_language(RC)
configure_file(
    "${CMAKE_SOURCE_DIR}/resources/windows/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SwitchAlbumSync.rc"
    @ONLY
)
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/SwitchAlbumSync.rc"
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app.ico"
)
# Listing a .manifest as a source makes the MSVC linker merge it into the
# manifest it generates (Common Controls v6, DPI awareness, supported OS).
configure_file(
    "${CMAKE_SOURCE_DIR}/resources/windows/app.manifest.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SwitchAlbumSync.manifest"
    @ONLY
)

# Keep the Windows release as one .exe. Embed the notices and, for builds
# with Discord, the runtime that is extracted on first use.
file(TO_CMAKE_PATH "${NSO_THIRD_PARTY_NOTICES}" NSO_THIRD_PARTY_NOTICES_RC)
set(NSO_EMBEDDED_RESOURCES_RC "${CMAKE_CURRENT_BINARY_DIR}/embedded-resources.rc")
file(WRITE "${NSO_EMBEDDED_RESOURCES_RC}"
    "${NSO_THIRD_PARTY_NOTICES_RESOURCE_ID} RCDATA \"${NSO_THIRD_PARTY_NOTICES_RC}\"\n")
if(NSO_ENABLE_DISCORD)
    file(TO_CMAKE_PATH "${NSO_DISCORD_SDK_RUNTIME}" NSO_DISCORD_SDK_RUNTIME_RC)
    file(APPEND "${NSO_EMBEDDED_RESOURCES_RC}"
        "${NSO_DISCORD_SDK_RESOURCE_ID} RCDATA \"${NSO_DISCORD_SDK_RUNTIME_RC}\"\n")
endif()

list(APPEND NSO_SOURCES
    src/album_tools_windows.cpp
    src/annotate_windows.cpp
    src/auth_callback_windows.cpp
    src/backup_windows.cpp
    src/dashboard_windows.cpp
    src/system_status_windows.cpp
    src/text_search_windows.cpp
    src/video_windows.cpp
    src/library_windows.cpp
    src/import_windows.cpp
    src/share_windows.cpp
    src/theme_windows.cpp
    src/http_windows.cpp
    src/platform_windows.cpp
    src/secure_store_windows.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/SwitchAlbumSync.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/SwitchAlbumSync.manifest"
    "${NSO_EMBEDDED_RESOURCES_RC}"
)

add_executable(SwitchAlbumSync WIN32 ${NSO_SOURCES} ${NSO_HEADERS})
set_target_properties(SwitchAlbumSync PROPERTIES OUTPUT_NAME "Switch Album Sync")

target_include_directories(SwitchAlbumSync PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(
    SwitchAlbumSync
    PRIVATE
        NSO_APP_VERSION="${PROJECT_VERSION}"
        NSO_NXAPI_AUTH_CLIENT_ID="${NSO_NXAPI_AUTH_CLIENT_ID}"
        NSO_ZNCA_CLIENT_VERSION="${NSO_ZNCA_CLIENT_VERSION}"
        NSO_ENABLE_DISCORD=$<BOOL:${NSO_ENABLE_DISCORD}>
        NSO_THIRD_PARTY_NOTICES_RESOURCE_ID=${NSO_THIRD_PARTY_NOTICES_RESOURCE_ID}
)
if(NSO_ENABLE_DISCORD)
    target_include_directories(SwitchAlbumSync PRIVATE "${NSO_DISCORD_SDK_INCLUDE_DIR}")
    target_compile_definitions(
        SwitchAlbumSync
        PRIVATE
            NSO_DISCORD_SOCIAL_SDK_VERSION="${NSO_DISCORD_SOCIAL_SDK_VERSION}"
            NSO_DISCORD_SDK_RESOURCE_ID=${NSO_DISCORD_SDK_RESOURCE_ID}
    )
    target_link_libraries(SwitchAlbumSync PRIVATE "${NSO_DISCORD_SDK_LINK_LIBRARY}")
    if(MSVC)
        target_link_options(SwitchAlbumSync PRIVATE /DELAYLOAD:discord_partner_sdk.dll)
        target_link_libraries(SwitchAlbumSync PRIVATE delayimp)
    endif()
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT NSO_IPO_SUPPORTED OUTPUT NSO_IPO_ERROR LANGUAGES CXX)
if(NSO_IPO_SUPPORTED)
    set_property(TARGET SwitchAlbumSync PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "IPO/LTO disabled: ${NSO_IPO_ERROR}")
endif()

target_link_libraries(
    SwitchAlbumSync
    PRIVATE advapi32 bcrypt crypt32 winhttp shell32 ole32 comdlg32 comctl32 gdi32 uuid gdiplus dwmapi uxtheme mfplat mfplay mfuuid
            mfreadwrite windowscodecs windowsapp
)
# C++/WinRT (Windows' on-device text recognition) needs large object files.
if(MSVC)
    set_source_files_properties(src/text_search_windows.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

install(TARGETS SwitchAlbumSync RUNTIME DESTINATION bin)

option(NSO_BUILD_TESTS "Build Windows capture feature regression tests" OFF)
if(NSO_BUILD_TESTS)
    enable_testing()
    set(NSO_TEST_SOURCES ${NSO_SOURCES})
    list(REMOVE_ITEM NSO_TEST_SOURCES src/main.cpp)
    add_executable(WindowsFeatureTests tests/windows_features.cpp ${NSO_TEST_SOURCES})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES LINK_OPTIONS)
        get_target_property(value SwitchAlbumSync ${property})
        if(value)
            set_property(TARGET WindowsFeatureTests PROPERTY ${property} "${value}")
        endif()
    endforeach()
    target_include_directories(WindowsFeatureTests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # The HTTP regression test runs a loopback server.
    target_link_libraries(WindowsFeatureTests PRIVATE ws2_32)
    add_test(NAME WindowsCaptureFeatures COMMAND WindowsFeatureTests)

    # Runs the real API clients against a loopback mock of Nintendo and nxapi
    # that checks the nxapi-znca-api public API terms. For a long run with a
    # report: ApiUsageTests --soak 600 --report <dir>
    add_executable(ApiUsageTests tests/api_usage.cpp tests/mock_nintendo_api.hpp ${NSO_TEST_SOURCES})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES LINK_OPTIONS)
        get_target_property(value SwitchAlbumSync ${property})
        if(value)
            set_property(TARGET ApiUsageTests PROPERTY ${property} "${value}")
        endif()
    endforeach()
    target_include_directories(ApiUsageTests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(ApiUsageTests PRIVATE ws2_32)
    add_test(NAME ApiUsageCompliance COMMAND ApiUsageTests)
endif()
