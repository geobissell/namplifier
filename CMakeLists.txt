cmake_minimum_required(VERSION 3.22)

project(Namplifier VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(NAM_ENABLE_A2_FAST "Build NAM A2 fast-path WaveNet" ON)
if(NAM_ENABLE_A2_FAST)
  add_compile_definitions(NAM_ENABLE_A2_FAST)
endif()

set(Namplifier_TONE3000_KEY "" CACHE STRING "Tone3000 publishable API key (t3k_pk_...)")
if(DEFINED ENV{Namplifier_TONE3000_KEY} AND NOT Namplifier_TONE3000_KEY)
  set(Namplifier_TONE3000_KEY "$ENV{Namplifier_TONE3000_KEY}" CACHE STRING "" FORCE)
endif()

set(JUCE_WEBVIEW2_PACKAGE_LOCATION "$ENV{LOCALAPPDATA}/PackageManagement/NuGet/Packages" CACHE PATH "WebView2 NuGet packages root")

# ASIO headers (Steinberg SDK subset vendored for JUCE_ASIO; see third_party/asiosdk/LICENSE.txt)
set(Namplifier_HAS_ASIO FALSE)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common/iasiodrv.h")
  set(Namplifier_HAS_ASIO TRUE)
  message(STATUS "ASIO enabled via third_party/asiosdk")
endif()

include(FetchContent)
FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.6
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

set(NAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore")
set(ADT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/AudioDSPTools")
set(EIGEN_ROOT "${NAM_ROOT}/Dependencies/eigen")
set(NLOHMANN_ROOT "${NAM_ROOT}/Dependencies/nlohmann")

# MSVC drops unused static registrars from nam_core — keep WaveNet / SlimmableContainer linked.
set(_nam_force_link_patch "${CMAKE_CURRENT_SOURCE_DIR}/third_party/patches/NeuralAmpModelerCore-force-link.patch")
if(EXISTS "${NAM_ROOT}/NAM/container.h" AND EXISTS "${_nam_force_link_patch}")
  file(READ "${NAM_ROOT}/NAM/container.h" _nam_container_h)
  if(NOT _nam_container_h MATCHES "force_link_slimmable_container")
    find_package(Git REQUIRED)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_nam_force_link_patch}"
      WORKING_DIRECTORY "${NAM_ROOT}"
      RESULT_VARIABLE _nam_patch_rc
      ERROR_VARIABLE _nam_patch_err
    )
    if(NOT _nam_patch_rc EQUAL 0)
      message(FATAL_ERROR "Failed to apply NAM force-link patch: ${_nam_patch_err}")
    endif()
    message(STATUS "Applied third_party/patches/NeuralAmpModelerCore-force-link.patch")
  endif()
endif()

file(GLOB_RECURSE NAM_SOURCES CONFIGURE_DEPENDS
  "${NAM_ROOT}/NAM/*.cpp"
  "${NAM_ROOT}/NAM/wavenet/*.cpp"
)

set(ADT_SOURCES
  "${ADT_ROOT}/dsp/dsp.cpp"
  "${ADT_ROOT}/dsp/ImpulseResponse.cpp"
  "${ADT_ROOT}/dsp/RecursiveLinearFilter.cpp"
  "${ADT_ROOT}/dsp/wav.cpp"
)

add_library(nam_core STATIC ${NAM_SOURCES})
target_include_directories(nam_core PUBLIC
  "${NAM_ROOT}/NAM"
  "${EIGEN_ROOT}"
  "${NLOHMANN_ROOT}"
)
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)
if(MSVC)
  target_compile_options(nam_core PRIVATE /bigobj /wd4244 /wd4267 /wd4018)
endif()

add_library(audio_dsp_tools STATIC ${ADT_SOURCES})
target_include_directories(audio_dsp_tools PUBLIC
  "${ADT_ROOT}/dsp"
  "${ADT_ROOT}/dsp/ResamplingContainer"
  "${ADT_ROOT}/dsp/ResamplingContainer/Dependencies"
  "${EIGEN_ROOT}"
)
# AudioDSPTools ImpulseResponse headers still use double** in places — keep default double DSP_SAMPLE
target_link_libraries(audio_dsp_tools PUBLIC nam_core)
if(MSVC)
  target_compile_options(audio_dsp_tools PRIVATE /wd4244 /wd4267)
endif()

set(UI_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist")
if(NOT EXISTS "${UI_DIST_DIR}/index.html")
  file(MAKE_DIRECTORY "${UI_DIST_DIR}")
  file(WRITE "${UI_DIST_DIR}/index.html" "<!DOCTYPE html><html><body><p>Build the UI with npm run build in ui/</p></body></html>")
endif()

juce_add_binary_data(NamplifierUiData SOURCES
  "${UI_DIST_DIR}/index.html"
)

# Collect UI assets if present (js/css) for BinaryData
file(GLOB UI_ASSETS CONFIGURE_DEPENDS
  "${UI_DIST_DIR}/assets/*"
)
if(UI_ASSETS)
  juce_add_binary_data(NamplifierUiAssets SOURCES ${UI_ASSETS})
  set(UI_ASSET_LIB NamplifierUiAssets)
else()
  set(UI_ASSET_LIB "")
endif()

juce_add_plugin(Namplifier
  COMPANY_NAME "Namplifier"
  BUNDLE_ID com.namplifier.plugin
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE
  PLUGIN_MANUFACTURER_CODE Namp
  PLUGIN_CODE Namp
  FORMATS VST3 Standalone
  PRODUCT_NAME "Namplifier"
  NEEDS_WEBVIEW2 TRUE
)

target_sources(Namplifier PRIVATE
  plugin/Source/PluginProcessor.cpp
  plugin/Source/PluginProcessor.h
  plugin/Source/PluginEditor.cpp
  plugin/Source/PluginEditor.h
  plugin/Source/dsp/GraphEngine.cpp
  plugin/Source/dsp/GraphEngine.h
  plugin/Source/dsp/GraphModel.cpp
  plugin/Source/dsp/GraphModel.h
  plugin/Source/dsp/Nodes.cpp
  plugin/Source/dsp/Nodes.h
  plugin/Source/dsp/NamForceLink.cpp
  plugin/Source/dsp/ResamplingNAM.h
  plugin/Source/tone3000/Tone3000Client.cpp
  plugin/Source/tone3000/Tone3000Client.h
  plugin/Source/bridge/NativeBridge.cpp
  plugin/Source/bridge/NativeBridge.h
  plugin/Source/presets/PresetManager.cpp
  plugin/Source/presets/PresetManager.h
  plugin/Source/presets/LibraryManager.cpp
  plugin/Source/presets/LibraryManager.h
)

target_compile_definitions(Namplifier
  PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    NAMPLIFIER_TONE3000_KEY="${Namplifier_TONE3000_KEY}"
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
)
if(Namplifier_HAS_ASIO)
  target_compile_definitions(Namplifier PUBLIC JUCE_ASIO=1)
  target_include_directories(Namplifier PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common")
endif()

target_include_directories(Namplifier PRIVATE
  plugin/Source
  "${NAM_ROOT}/NAM"
  "${ADT_ROOT}/dsp"
  "${ADT_ROOT}/dsp/ResamplingContainer"
  "${ADT_ROOT}/dsp/ResamplingContainer/Dependencies"
  "${EIGEN_ROOT}"
  "${NLOHMANN_ROOT}"
)

target_link_libraries(Namplifier
  PRIVATE
    nam_core
    NamplifierUiData
    ${UI_ASSET_LIB}
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_cryptography
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# NAM registers architectures via static initializers; MSVC drops those from .lib unless forced.
if(MSVC)
  if(TARGET Namplifier)
    target_link_options(Namplifier PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:nam_core>")
  endif()
  if(TARGET Namplifier_Standalone)
    target_link_options(Namplifier_Standalone PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:nam_core>")
  endif()
  if(TARGET Namplifier_VST3)
    target_link_options(Namplifier_VST3 PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:nam_core>")
  endif()
endif()

if(MSVC)
  target_compile_options(Namplifier PRIVATE /wd4244 /wd4267 /wd4996)
endif()

juce_generate_juce_header(Namplifier)

# Amp sims need live input — override JUCE's muted-input standalone default.
if(TARGET Namplifier_Standalone)
  target_compile_definitions(Namplifier_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
  target_sources(Namplifier_Standalone PRIVATE plugin/Source/StandaloneApp.cpp)
endif()

