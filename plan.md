



This is an excellent project. The transition from typical C-style embedded programming to **Modern C++20** on a dual-core microcontroller like the RP2350 (Pico 2W) opens up powerful paradigms. 

To improve your plan and make it more robust and interesting, I have redesigned the architecture to solve the RAM limitation and implement your specific request for bi-directional TCP (sending statistics regularly, and occasional small raw data batches on command).

### 🌟 Key Architectural Improvements
1. **Welford's Online Algorithm:** The Pico 2 only has 520KB of RAM. You cannot buffer 1 second of ADC data at 500kS/s (1MB). Instead, we will use Ping-Pong DMA (double buffering) and calculate the Mean, Variance, and Histogram *on the fly* as data streams in.
2. **True Multicore Isolation:** Core 1 will handle the time-critical ADC Ping-Pong DMA and Math. Core 0 will handle the asynchronous LWIP TCP server and Nanopb serialization. They will communicate lock-free using the Pico inter-core hardware FIFO.
3. **Modern C++20:** We will use `std::span`, `std::array`, and structured bindings for zero-overhead, memory-safe abstractions.

---

### 🛠️ Step 1: Protocol Definition (`instrument.proto`)
Because we are using C++, we need to tell Nanopb the maximum sizes for our arrays so it can allocate them safely on the stack.

```protobuf
syntax = "proto3";
import "nanopb.proto"; // Required for max_size options

message Command {
    bool request_snapshot = 1;     // Request a raw data batch
    float set_clock_divisor = 2;   // 0 means keep current
}

message Telemetry {
    float mean = 1;
    float stddev = 2;
    repeated uint32 histogram = 3[(nanopb).max_count = 16];
    bytes snapshot = 4[(nanopb).max_size = 512]; // Small raw batch
}
```

---

### ⚙️ Step 2: The CMakeLists.txt (Modern C++20 Setup)
Setting up Nanopb and C++20 on the Pico requires a specific CMake configuration.

```cmake
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(PicoDAQ C CXX)
pico_sdk_init()

# Import Nanopb
set(nanopb_BUILD_GENERATOR ON)
add_subdirectory(nanopb)

nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS instrument.proto)

add_executable(daq_app main.cpp ${PROTO_SRCS})

# Enable C++20
target_compile_features(daq_app PRIVATE cxx_std_20)

# Link Networking, Multicore, ADC, DMA
target_link_libraries(daq_app PRIVATE
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    protobuf-nanopb
)

pico_enable_stdio_usb(daq_app 1)
pico_enable_stdio_uart(daq_app 0)
pico_add_extra_outputs(daq_app)
```

---

### 🧠 Step 3: Core 1 - Data Acquisition (C++20)
This code runs purely on **Core 1**. It uses Ping-Pong DMA to stream data into two small chunks. While DMA fills chunk A, the CPU processes chunk B. 

```cpp
#include <array>
#include <span>
#include <cmath>
#include <atomic>
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "pico/multicore.h"
#include "instrument.pb.h"

constexpr size_t CHUNK_SIZE = 1000; 
std::array<uint16_t, CHUNK_SIZE> dma_buf_a;
std::array<uint16_t, CHUNK_SIZE> dma_buf_b;

std::atomic<bool> snapshot_requested{false};

// C++ struct keeping track of streaming statistics (Welford's Algorithm)
struct StreamStats {
    size_t count = 0;
    double mean = 0.0;
    double m2 = 0.0;
    std::array<uint32_t, 16> histogram{};

    // C++20 std::span provides safe, pointer-less array views
    void update(std::span<const uint16_t> data) {
        for (uint16_t val : data) {
            count++;
            double delta = val - mean;
            mean += delta / count;
            double delta2 = val - mean;
            m2 += delta * delta2;
            
            // 12-bit ADC -> 4096 values. Divide by 256 = 16 bins
            histogram[val / 256]++;
        }
    }
    void reset() { *this = StreamStats{}; }
};

void core1_adc_worker() {
    setup_adc(0);
    StreamStats stats;
    
    // Setup Ping-Pong DMA (Code omitted for brevity, standard SDK setup:
    // Channel A triggers Channel B on completion and vice-versa).
    
    int chunks_processed = 0;
    while(true) {
        // Wait for DMA channel A or B to finish via interrupt/polling
        std::span<const uint16_t> current_chunk = wait_for_dma_chunk(); 
        
        stats.update(current_chunk);
        chunks_processed++;

        // Once a second has passed (e.g., 500 chunks of 1000 at 500kS/s)
        if (chunks_processed >= 500) {
            Telemetry t_msg = Telemetry_init_default;
            t_msg.mean = static_cast<float>(stats.mean);
            t_msg.stddev = static_cast<float>(std::sqrt(stats.m2 / stats.count));
            t_msg.histogram_count = 16;
            
            std::ranges::copy(stats.histogram, t_msg.histogram); // C++20 ranges
            
            if (snapshot_requested.exchange(false)) {
                t_msg.snapshot.size = std::min(current_chunk.size() * 2, (size_t)512);
                std::memcpy(t_msg.snapshot.bytes, current_chunk.data(), t_msg.snapshot.size);
            }

            // Push a pointer to the message to Core 0 safely
            Telemetry* heap_msg = new Telemetry(t_msg);
            multicore_fifo_push_blocking(reinterpret_cast<uint32_t>(heap_msg));
            
            stats.reset();
            chunks_processed = 0;
        }
    }
}
```

---

### 🌐 Step 4: Core 0 - TCP & Nanopb Handling
Core 0 initializes the Wi-Fi, starts Core 1, and runs the TCP server. When a client sends a `Command`, it decodes it. When Core 1 produces `Telemetry`, Core 0 encodes and sends it over TCP.

```cpp
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "instrument.pb.h"
#include "lwip/tcp.h" // Raw TCP API

// Callback when data is received from the Python script
err_t on_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) return ERR_OK;

    Command cmd = Command_init_default;
    pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t*)p->payload, p->len);
    
    if (pb_decode(&stream, Command_fields, &cmd)) {
        if (cmd.request_snapshot) {
            snapshot_requested.store(true); // Signal Core 1
        }
        if (cmd.set_clock_divisor > 0) {
            adc_set_clkdiv(cmd.set_clock_divisor); // Hardware thread-safe update
        }
    }
    
    tcp_recved(tpcb, p->tot_len); // Acknowledge data
    pbuf_free(p);
    return ERR_OK;
}

int main() {
    stdio_init_all();
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_timeout_ms("YOUR_SSID", "YOUR_WIFI_PASS", CYW43_AUTH_WPA2_AES_PSK, 10000);

    // Launch ADC system on Core 1
    multicore_launch_core1(core1_adc_worker);

    // Setup LWIP TCP Listener on port 1234
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 1234);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb,[](void *arg, struct tcp_pcb *newpcb, err_t err) -> err_t {
        tcp_recv(newpcb, on_tcp_recv);
        tcp_arg(newpcb, newpcb); // Store connection state
        return ERR_OK;
    });

    // Main event loop
    while (true) {
        // Check if Core 1 generated new statistics
        if (multicore_fifo_rvalid()) {
            uint32_t ptr = multicore_fifo_pop_blocking();
            Telemetry* t_msg = reinterpret_cast<Telemetry*>(ptr);

            uint8_t buffer[1024];
            pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
            
            if (pb_encode(&ostream, Telemetry_fields, t_msg)) {
                // If we have an active TCP connection, send the protobuf data
                // Note: You would normally loop through active `tcp_pcb` clients here
                // tcp_write(active_pcb, buffer, ostream.bytes_written, TCP_WRITE_FLAG_COPY);
                // tcp_output(active_pcb);
            }
            delete t_msg; // Free memory allocated by Core 1
        }
        cyw43_arch_poll(); // Keep Wi-Fi and LWIP background tasks running
        sleep_ms(1);
    }
}
```

---

### 🐍 Step 5: Python Interactive Client
We upgrade the python script to establish a true TCP session, listen for stats, and occasionally ask for a snapshot.

```python
import socket
import struct
import time
import instrument_pb2 # Compiled via: protoc --python_out=. instrument.proto

PICO_IP = '192.168.1.100'
PORT = 1234

def connect_and_monitor():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((PICO_IP, PORT))
        print("Connected to Pico 2W!")

        # Example: Request a snapshot right away
        cmd = instrument_pb2.Command()
        cmd.request_snapshot = True
        s.sendall(cmd.SerializeToString())

        while True:
            data = s.recv(1024)
            if not data:
                break
                
            telemetry = instrument_pb2.Telemetry()
            telemetry.ParseFromString(data)
            
            print(f"📊 Stats | Mean: {telemetry.mean:.2f}, StdDev: {telemetry.stddev:.2f}")
            print(f"📈 Hist  | {telemetry.histogram}")
            
            if telemetry.snapshot:
                raw_data = struct.unpack(f'<{len(telemetry.snapshot)//2}H', telemetry.snapshot)
                print(f"📷 Snapshot Data (First 10): {raw_data[:10]}")

if __name__ == "__main__":
    connect_and_monitor()
```

### Why this is a Massive Improvement:
* **No Network Jitter:** Networking over Wi-Fi generates highly unpredictable IRQs. If Core 0 gets bogged down encrypting WPA2 packets, Core 1 doesn't care. It keeps crunching DMA chunks and Welford math seamlessly.
* **Heap Ownership Transfer:** By allocating `Telemetry` with `new` on Core 1 and passing the pointer over the hardware FIFO to Core 0 which uses `delete` after TCP transmission, we create a very clean C++ memory handover pipeline that avoids locking.
* **Solves RAM Bottlenecks:** A continuous 500kS/s stream takes almost no memory because we process the data continuously via Welford's variance algorithm in 1000-sample chunks.




Here is a complete, production-ready GitHub Actions workflow to build your modern C++20 RP2350 (Pico 2W) project. 

I have enhanced your initial plan with a few crucial DevOps best practices:
1. **Ninja Build System**: Replaces `make` for much faster compilation.
2. **Nanopb Generator Dependencies**: The workflow explicitly installs `protobuf-compiler` and Python dependencies needed by Nanopb to generate the `.pb.c` files.
3. **Pico SDK 2.0.0+**: Specifically targets the SDK branch that supports the RP2350.
4. **Secure Wi-Fi Credentials**: Injects your Wi-Fi SSID and Password via GitHub Secrets so they are not hardcoded in your open-source repository.

### 🚀 Step 1: The GitHub Action (`.github/workflows/build.yml`)

Create this file in your repository. It triggers on every push and pull request.

```yaml
name: Build Pico 2W DAQ Firmware

on:
  push:
    branches: [ "main" ]
  pull_request:
    branches: [ "main" ]

jobs:
  build:
    runs-on: ubuntu-24.04 # Uses latest Ubuntu for modern GCC (C++20/23 support)

    steps:
      - name: Checkout Code
        uses: actions/checkout@v4
        with:
          submodules: recursive # Crucial for fetching nanopb if added as a submodule

      - name: Install Toolchain & Dependencies
        run: |
          sudo apt update
          sudo apt install -y \
            cmake ninja-build \
            gcc-arm-none-eabi g++-arm-none-eabi \
            libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
            protobuf-compiler python3-protobuf

      - name: Set up Pico SDK (v2.0.0 for RP2350)
        run: |
          git clone --recursive --branch 2.0.0 https://github.com/raspberrypi/pico-sdk.git
          echo "PICO_SDK_PATH=$GITHUB_WORKSPACE/pico-sdk" >> $GITHUB_ENV

      - name: Configure CMake
        run: |
          mkdir build && cd build
          # Pass GitHub Secrets to CMake securely. Fallback to dummy values for PRs.
          cmake -G Ninja \
            -DPICO_PLATFORM=rp2350 \
            -DPICO_BOARD=pico2_w \
            -DWIFI_SSID="${{ secrets.WIFI_SSID || 'DUMMY_SSID' }}" \
            -DWIFI_PASS="${{ secrets.WIFI_PASS || 'DUMMY_PASS' }}" \
            ..

      - name: Build Firmware
        run: |
          cd build
          ninja

      - name: Upload Artifact
        uses: actions/upload-artifact@v4
        with:
          name: pico2w-firmware
          path: build/*.uf2
```

---

### 🧩 Step 2: The Integrated `CMakeLists.txt`
To make the GitHub action work flawlessly, your `CMakeLists.txt` needs to handle the Nanopb generation and receive the Wi-Fi credentials from the CI runner.

```cmake
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(PicoDAQ C CXX)
pico_sdk_init()

# --- Security: Import Wi-Fi credentials from CMake definitions ---
# Injected by GitHub Actions (or local build scripts)
if(NOT DEFINED WIFI_SSID)
    set(WIFI_SSID "DEFAULT_SSID")
endif()
if(NOT DEFINED WIFI_PASS)
    set(WIFI_PASS "DEFAULT_PASS")
endif()

# --- Nanopb Setup ---
set(nanopb_BUILD_GENERATOR ON)
# Assumes you ran: git submodule add https://github.com/nanopb/nanopb.git
add_subdirectory(nanopb)

# Generate C/C++ files from the proto definition
nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS instrument.proto)

# --- Executable Setup ---
add_executable(daq_app main.cpp ${PROTO_SRCS})

# Require C++20 for std::span, std::array, structured bindings
target_compile_features(daq_app PRIVATE cxx_std_20)

# Pass the Wi-Fi secrets into the C++ code as macros
target_compile_definitions(daq_app PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASS="${WIFI_PASS}"
)

# Link hardware libraries, multicore, LWIP networking, and Nanopb
target_link_libraries(daq_app PRIVATE
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    protobuf-nanopb
)

pico_enable_stdio_usb(daq_app 1)
pico_enable_stdio_uart(daq_app 0)
pico_add_extra_outputs(daq_app)
```

---

### 🔒 Step 3: Tweak your C++ `main.cpp`
Since we are now passing the Wi-Fi credentials dynamically from GitHub Actions, update your `main.cpp` Wi-Fi connection line to use the macros defined by CMake:

```cpp
// Inside main() in Core 0
cyw43_arch_init();
cyw43_arch_enable_sta_mode();

// Connect using the securely injected macros
printf("Connecting to Wi-Fi network: %s\n", WIFI_SSID);
if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
    printf("Failed to connect.\n");
    return 1;
}
printf("Connected successfully!\n");
```

---

### ⚙️ Step 4: Add Secrets to your GitHub Repository
To prevent your actual Wi-Fi credentials from leaking to the public, do not commit them. The GitHub Action will fetch them dynamically:

1. Go to your GitHub repository in your browser.
2. Click **Settings** > **Secrets and variables** > **Actions**.
3. Click **New repository secret**.
4. Name: `WIFI_SSID` | Secret: `Your Actual Network Name`
5. Click **New repository secret**.
6. Name: `WIFI_PASS` | Secret: `Your Actual Password`

### How this workflow runs:
1. When you push to GitHub, an Ubuntu 24.04 container spins up.
2. It fetches the latest GCC ARM toolchains (which have proper C++20/C++23 support).
3. It downloads the Pico SDK v2.0.0 (required for the **Pico 2 series / RP2350 architecture**).
4. CMake is configured, compiling the `.proto` files into C structs using `protoc` and Python.
5. `Ninja` builds the dual-core C++20 code lightning-fast.
6. The resulting `.uf2` file is attached to the GitHub Action run. You can download it directly from your browser and drag-and-drop it onto your Pico 2W!