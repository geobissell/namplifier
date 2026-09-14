# ASIO SDK (optional)

Namplifier enables ASIO when this folder contains the Steinberg ASIO SDK
(`common/iasiodrv.h`).

Do **not** commit the SDK unless your license allows it.

1. Download from https://www.steinberg.net/asiosdk
2. Extract so you have: `third_party/asiosdk/common/iasiodrv.h`
3. Reconfigure CMake — you should see `ASIO enabled via third_party/asiosdk`

Without the SDK, Namplifier still builds (WASAPI / other JUCE backends).
