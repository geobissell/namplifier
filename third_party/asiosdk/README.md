# ASIO SDK (Windows)

Steinberg ASIO SDK headers used with `JUCE_ASIO=1`.

This project is AGPL-3.0, so we use the SDK under its **GPLv3** dual-license option
(see `LICENSE.txt`). Headers in `common/` are committed so CI Windows builds include ASIO.

If headers are missing, CMake fetches https://github.com/audiosdk/asio automatically.
