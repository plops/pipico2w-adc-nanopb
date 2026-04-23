#include <cstdio>
#include <array>
#include <span>
#include <cmath>
#include <atomic>
#include <cstring>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "instrument.pb.h"
#include "lwip/tcp.h"

// --- Configuration ---
constexpr size_t CHUNK_SIZE = 1000;
constexpr uint ADC_PIN = 26; // ADC0
constexpr uint ADC_NUM = 0;
constexpr float ADC_SAMPLE_RATE = 500000.0f;

// --- Buffers ---
alignas(4) std::array<uint16_t, CHUNK_SIZE> dma_buf_a;
alignas(4) std::array<uint16_t, CHUNK_SIZE> dma_buf_b;

// --- Synchronization ---
std::atomic<bool> snapshot_requested{false};
std::atomic<int> ready_chunk{-1}; // -1: none, 0: buf_a, 1: buf_b
int dma_chan_a, dma_chan_b;

// --- TCP State ---
struct tcp_pcb *active_pcb = nullptr;

// --- Welford's Algorithm for Online Statistics ---
struct StreamStats {
    size_t count = 0;
    double mean = 0.0;
    double m2 = 0.0;
    std::array<uint32_t, 16> histogram{};

    void update(std::span<const uint16_t> data) {
        for (uint16_t val : data) {
            count++;
            double delta = val - mean;
            mean += delta / count;
            double delta2 = val - mean;
            m2 += delta * delta2;
            
            // 12-bit ADC -> 4096 values. Divide by 256 = 16 bins
            histogram[std::clamp<uint16_t>(val / 256, 0, 15)]++;
        }
    }

    void reset() {
        count = 0;
        mean = 0.0;
        m2 = 0.0;
        histogram.fill(0);
    }
};

// --- DMA Completion Handlers ---
void __isr dma_handler() {
    if (dma_hw->ints0 & (1u << dma_chan_a)) {
        dma_hw->ints0 = (1u << dma_chan_a); // Clear interrupt
        ready_chunk.store(0);
    }
    if (dma_hw->ints0 & (1u << dma_chan_b)) {
        dma_hw->ints0 = (1u << dma_chan_b); // Clear interrupt
        ready_chunk.store(1);
    }
}

// --- ADC & DMA Setup ---
void setup_adc_dma() {
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_NUM);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(48000000.0f / ADC_SAMPLE_RATE - 1.0f);

    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config c_a = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&c_a, DMA_SIZE_16);
    channel_config_set_read_increment(&c_a, false);
    channel_config_set_write_increment(&c_a, true);
    channel_config_set_dreq(&c_a, DREQ_ADC);
    channel_config_set_chain_to(&c_a, dma_chan_b);

    dma_channel_configure(dma_chan_a, &c_a, dma_buf_a.data(), &adc_hw->fifo, CHUNK_SIZE, false);

    dma_channel_config c_b = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&c_b, DMA_SIZE_16);
    channel_config_set_read_increment(&c_b, false);
    channel_config_set_write_increment(&c_b, true);
    channel_config_set_dreq(&c_b, DREQ_ADC);
    channel_config_set_chain_to(&c_b, dma_chan_a);

    dma_channel_configure(dma_chan_b, &c_b, dma_buf_b.data(), &adc_hw->fifo, CHUNK_SIZE, false);

    dma_set_irq0_channel_mask_enabled((1u << dma_chan_a) | (1u << dma_chan_b), true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    adc_run(true);
    dma_start_channel_mask(1u << dma_chan_a);
}

// --- Core 1: Data Acquisition Worker ---
void core1_adc_worker() {
    setup_adc_dma();
    StreamStats stats;
    int chunks_processed = 0;

    while (true) {
        int chunk_idx = -1;
        while ((chunk_idx = ready_chunk.exchange(-1)) == -1) {
            tight_loop_contents();
        }

        std::span<const uint16_t> current_chunk = (chunk_idx == 0) ? dma_buf_a : dma_buf_b;
        
        // Re-arm the DMA channel that just finished (it already chained, but we need to reset its write address for next time)
        if (chunk_idx == 0) {
            dma_channel_set_write_addr(dma_chan_a, dma_buf_a.data(), false);
        } else {
            dma_channel_set_write_addr(dma_chan_b, dma_buf_b.data(), false);
        }

        stats.update(current_chunk);
        chunks_processed++;

        // Every 500 chunks (~1 second at 500kS/s)
        if (chunks_processed >= 500) {
            Telemetry* t_msg = new Telemetry(Telemetry_init_default);
            t_msg->mean = static_cast<float>(stats.mean);
            t_msg->stddev = static_cast<float>(std::sqrt(stats.m2 / (stats.count > 1 ? stats.count - 1 : 1)));
            t_msg->histogram_count = 16;
            std::copy(stats.histogram.begin(), stats.histogram.end(), t_msg->histogram);

            if (snapshot_requested.exchange(false)) {
                size_t bytes_to_copy = std::min(current_chunk.size_bytes(), (size_t)512);
                t_msg->snapshot.size = bytes_to_copy;
                std::memcpy(t_msg->snapshot.bytes, current_chunk.data(), bytes_to_copy);
            }

            // Push pointer to Core 0
            multicore_fifo_push_blocking(reinterpret_cast<uint32_t>(t_msg));
            
            stats.reset();
            chunks_processed = 0;
        }
    }
}

// --- Core 0: TCP Server Logic ---
err_t on_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        active_pcb = nullptr;
        return ERR_OK;
    }

    Command cmd = Command_init_default;
    pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t*)p->payload, p->len);
    
    if (pb_decode(&stream, Command_fields, &cmd)) {
        if (cmd.request_snapshot) {
            snapshot_requested.store(true);
        }
        if (cmd.set_clock_divisor > 0) {
            adc_set_clkdiv(cmd.set_clock_divisor);
        }
    }
    
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

err_t on_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    active_pcb = newpcb;
    tcp_recv(newpcb, on_tcp_recv);
    return ERR_OK;
}

int main() {
    stdio_init_all();
    
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return -1;
    }

    cyw43_arch_enable_sta_mode();
    printf("Connecting to Wi-Fi: %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Wi-Fi connection failed\n");
        return -1;
    }
    printf("Connected! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    multicore_launch_core1(core1_adc_worker);

    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 1234);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, on_tcp_accept);

    uint8_t proto_buffer[1024];

    while (true) {
        if (multicore_fifo_rvalid()) {
            uint32_t ptr = multicore_fifo_pop_blocking();
            Telemetry* t_msg = reinterpret_cast<Telemetry*>(ptr);

            if (active_pcb) {
                pb_ostream_t ostream = pb_ostream_from_buffer(proto_buffer, sizeof(proto_buffer));
                if (pb_encode(&ostream, Telemetry_fields, t_msg)) {
                    tcp_write(active_pcb, proto_buffer, ostream.bytes_written, TCP_WRITE_FLAG_COPY);
                    tcp_output(active_pcb);
                }
            }

            delete t_msg;
        }
        cyw43_arch_poll();
        sleep_ms(1);
    }
}
