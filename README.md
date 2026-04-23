# Pico 2W DAQ

A high-performance, dual-core data acquisition (DAQ) system built for the Raspberry Pi Pico 2W (RP2350 platform). 

This project demonstrates how to transition from typical C-style embedded programming to **Modern C++20** on a dual-core microcontroller, allowing for powerful paradigms, memory-safe abstractions, and real-time data processing without hitting RAM limits.

## 🌟 Key Features

*   **True Multicore Isolation**: 
    *   **Core 1** handles the time-critical ADC Ping-Pong DMA and Math.
    *   **Core 0** handles the asynchronous LWIP TCP server and Nanopb serialization. 
    *   They communicate safely and lock-free using the Pico inter-core hardware FIFO.
*   **Welford's Online Algorithm**: The Pico 2W has limited RAM (520KB), making buffering 1 second of ADC data at 500kS/s (1MB) impossible. Instead, we use Ping-Pong DMA (double buffering) and calculate the Mean, Variance, and Histogram *on the fly* as data streams in.
*   **Protocol Buffers (Nanopb)**: Uses Nanopb for efficient binary serialization of commands and telemetry over a raw TCP socket, entirely bypassing network jitter on the data acquisition core.
*   **Modern C++20**: Extensively utilizes `std::span`, `std::array`, and structured bindings for zero-overhead, memory-safe abstractions.
*   **Automated CI/CD**: Fully integrated GitHub Actions workflow that automatically builds the firmware (`.uf2` file) on every push using Ninja and the latest Pico SDK.

## 🛠️ Hardware Requirements

*   Raspberry Pi Pico 2W (RP2350 platform)

## 🚀 Getting Started

### Prerequisites

To build the firmware locally, you will need the following dependencies installed:

*   CMake and Ninja
*   ARM GCC Toolchain (`gcc-arm-none-eabi`, `g++-arm-none-eabi`)
*   Protobuf Compiler and Python dependencies (`protobuf-compiler`, `python3-protobuf`)
*   [Pico SDK](https://github.com/raspberrypi/pico-sdk) (v2.0.0 or newer)

### Building the Firmware

1. Clone the repository and its submodules (Nanopb):
   ```bash
   git clone --recursive https://github.com/plops/pipico2w-adc-nanopb.git
   cd pipico2w-adc-nanopb
   ```

2. Set the `PICO_SDK_PATH` environment variable if you haven't already:
   ```bash
   export PICO_SDK_PATH=/path/to/pico-sdk
   ```

3. Configure the build with your Wi-Fi credentials. These will be securely injected into the C++ code as macros:
   ```bash
   mkdir build && cd build
   cmake -G Ninja \
     -DPICO_PLATFORM=rp2350 \
     -DPICO_BOARD=pico2_w \
     -DWIFI_SSID="YOUR_SSID" \
     -DWIFI_PASS="YOUR_WIFI_PASS" \
     ..
   ```

4. Build the project:
   ```bash
   ninja
   ```

5. Flash the firmware:
   Hold the `BOOTSEL` button on your Pico 2W while plugging it in via USB, then copy the generated `build/daq_app.uf2` file to the `RPI-RP2` drive.

## 📡 Communication Protocol

The device acts as a TCP server on port `1234`. The protocol is defined using Protocol Buffers in `instrument.proto`.

*   **Telemetry**: Every second, the Pico 2W streams a `Telemetry` message containing the calculated mean, standard deviation, and a 16-bin histogram of the data. 
*   **Commands**: You can send a `Command` message to the device to adjust the ADC clock divisor or request a raw data "snapshot" in the next telemetry packet.

## 🔒 CI/CD & Security

This repository includes a GitHub Actions workflow that automatically compiles the `.uf2` binary. 

To use it in a fork, you must add your Wi-Fi credentials as **GitHub Repository Secrets**:
*   `WIFI_SSID`
*   `WIFI_PASS`

This ensures your credentials are not leaked into the open-source codebase while allowing the CI runner to build working binaries.
