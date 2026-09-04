cmake_minimum_required(VERSION 3.20)
project(Monologue97SAPI CXX)

# The engine (mnvox11.dll) is a 32-bit PE, so mono_host.exe, the engine wrapper
# and the configuration utility are x86 only. The SAPI dll is built for both
# architectures and both builds drive that one 32-bit helper.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
                          WIN32_LEAN_AND_MEAN NOMINMAX)
  # Static CRT: the installer then carries no VC++ redistributable, and the
  # SAPI dll cannot fail to load inside a host that lacks one.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(MONO_ARCH "x64")
else()
  set(MONO_ARCH "x86")
endif()
message(STATUS "Monologue97 building for ${MONO_ARCH}")

# Every shipped binary carries a version resource naming itself, so a file
# found on disk later can be traced back to a release.
function(mono_version target desc filename)
  target_sources(${target} PRIVATE src/mono_version.rc)
  target_compile_definitions(${target} PRIVATE
    "MONO_FILE_DESC=\"${desc}\"" "MONO_FILE_NAME=\"${filename}\"")
endfunction()

# --- SAPI5 engine dll (both architectures) ---------------------------------
add_library(monologue_sapi SHARED
  src/mono_sapi.cpp
  src/mono_client.cpp
  src/mono_sapi.def
)
target_link_libraries(monologue_sapi PRIVATE ole32 oleaut32 advapi32 user32)
set_target_properties(monologue_sapi PROPERTIES
  OUTPUT_NAME "monologue_sapi_${MONO_ARCH}"
  LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/mono_sapi.def")
mono_version(monologue_sapi "Monologue 97 SAPI5 text-to-speech engine"
             "monologue_sapi_${MONO_ARCH}.dll")

# Ground-truth latency: WASAPI loopback timing of what actually reaches the
# speakers, which SAPI's own events under-report by ~60 ms. Built for both
# architectures, because 64-bit hosts take an extra process hop.
add_executable(loopback tools/loopback.cpp)
target_link_libraries(loopback PRIVATE ole32 oleaut32 sapi user32)
set_target_properties(loopback PROPERTIES OUTPUT_NAME "loopback_${MONO_ARCH}")

# Character-navigation regression: NVDA speaks single characters through
# <spell>, which arrives as SPVA_SpellOut, not SPVA_Speak.
add_executable(spell_test tools/spell_test.cpp)
target_link_libraries(spell_test PRIVATE ole32 oleaut32 sapi user32 advapi32)
set_target_properties(spell_test PROPERTIES
  OUTPUT_NAME "spell_test_${MONO_ARCH}")

# --- end-to-end SAPI test harness (both architectures) ---------------------
add_executable(sapi_test tools/sapi_test.cpp)
target_link_libraries(sapi_test PRIVATE ole32 oleaut32 sapi user32 advapi32)
set_target_properties(sapi_test PROPERTIES OUTPUT_NAME "sapi_test_${MONO_ARCH}")

# --- 32-bit engine helper, tools and configuration utility -----------------
if(MONO_ARCH STREQUAL "x86")
  add_executable(mono_host WIN32 src/mono_host.cpp src/mono_core.cpp)
  target_link_libraries(mono_host PRIVATE advapi32 user32)
  mono_version(mono_host "Monologue 97 speech helper" "mono_host.exe")

  add_executable(make_samples tools/make_samples.cpp src/mono_core.cpp)
  target_link_libraries(make_samples PRIVATE advapi32 user32)

  # The configuration utility drives the engine directly for its Test button,
  # so it is 32-bit too.
  add_executable(Monologue97Config WIN32
    config/mono_config.cpp
    config/mono_config.rc
    src/mono_core.cpp
  )
  target_link_libraries(Monologue97Config PRIVATE
    advapi32 user32 comctl32 gdi32 winmm ole32 shell32)
  target_include_directories(Monologue97Config PRIVATE src)
  mono_version(Monologue97Config "Monologue 97 configuration utility"
               "Monologue97Config.exe")

  # Latency measurement, engine-direct and through the helper.
  add_executable(bench tools/bench.cpp src/mono_core.cpp src/mono_client.cpp)
  target_link_libraries(bench PRIVATE advapi32 user32 ole32 oleaut32 sapi)

  # Robustness: no single character may take the helper down.
  add_executable(charfuzz tools/charfuzz.cpp src/mono_client.cpp)
  target_link_libraries(charfuzz PRIVATE advapi32 user32)

  # Accessibility verification for the configuration utility.
  add_executable(a11y_check tools/a11y_check.cpp)
  target_link_libraries(a11y_check PRIVATE oleacc ole32 oleaut32 user32)
endif()
