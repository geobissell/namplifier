cmake_minimum_required(VERSION 3.24)

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

file(GLOB_RECURSE UI_EMBED_FILES CONFIGURE_DEPENDS
  "${UI_DIST_DIR}/*"
)
list(FILTER UI_EMBED_FILES EXCLUDE REGEX "\\.(map)$")
if(NOT UI_EMBED_FILES)
  set(UI_EMBED_FILES "${UI_DIST_DIR}/index.html")
endif()

file(READ "${UI_DIST_DIR}/index.html" _namplifier_ui_index LIMIT 200)
if(_namplifier_ui_index MATCHES "Build the UI with npm")
  message(WARNING "ui/dist looks like a stub — run npm run build in ui/ before configuring so the plugin embeds the real UI.")
endif()

# Single BinaryData namespace so the editor can serve index + JS/CSS without a disk ui/dist.
juce_add_binary_data(NamplifierUiData
  HEADER_NAME NamplifierUiData.h
  NAMESPACE NamplifierUi
  SOURCES ${UI_EMBED_FILES}
)
set(UI_ASSET_LIB "")

set(Namplifier_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND Namplifier_FORMATS AU)
endif()

set(Namplifier_NEEDS_WEBVIEW2 FALSE)
if(WIN32)
  set(Namplifier_NEEDS_WEBVIEW2 TRUE)
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
  FORMATS ${Namplifier_FORMATS}
  PRODUCT_NAME "Namplifier"
  NEEDS_WEBVIEW2 ${Namplifier_NEEDS_WEBVIEW2}
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
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    NAMPLIFIER_TONE3000_KEY="${Namplifier_TONE3000_KEY}"
)
if(DEFINED NAMPLIFIER_BUILD_ID AND NOT NAMPLIFIER_BUILD_ID STREQUAL "")
  target_compile_definitions(Namplifier PUBLIC NAMPLIFIER_BUILD_ID="${NAMPLIFIER_BUILD_ID}")
endif()
if(WIN32)
  target_compile_definitions(Namplifier
    PUBLIC
      JUCE_USE_WIN_WEBVIEW2=1
      JUCE_WASAPI=1
      JUCE_DIRECTSOUND=0
  )
endif()
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
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_cryptography
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# NAM registers architectures via static initializers; linkers drop those from .a/.lib unless forced.
foreach (_nam_tgt IN ITEMS Namplifier_Standalone Namplifier_VST3 Namplifier_AU)
  if (TARGET ${_nam_tgt})
    target_link_libraries(${_nam_tgt} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")
  endif ()
endforeach ()

if(MSVC)
  target_compile_options(Namplifier PRIVATE /wd4244 /wd4267 /wd4996)
endif()

juce_generate_juce_header(Namplifier)

# Amp sims need live input — override JUCE's muted-input standalone default.
if(TARGET Namplifier_Standalone)
  target_compile_definitions(Namplifier_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
  target_sources(Namplifier_Standalone PRIVATE plugin/Source/StandaloneApp.cpp)
endif()

