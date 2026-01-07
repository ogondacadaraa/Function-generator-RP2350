#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "dac.pio.h"


#define DAC_BASE_PIN 0      
#define DAC_BITS     10     // 10-bit DAC
#define BUF_LEN      4096   // Length of waveform buffer must be power of 2
#define BUFFER_SIZE_BYTES (BUF_LEN * sizeof(uint16_t))

// Overclock target (kHz) my pcb could handle 384M max, some poeple they could get up to 700M, idk
#define OVERCLOCK_KHZ 384000 

uint16_t wave_buffer[BUF_LEN] __attribute__((aligned(BUFFER_SIZE_BYTES)));

#define PI 3.1415926535

enum WaveMode { SINE, TRIANGLE, SAWTOOTH, SQUARE };
WaveMode g_mode = SINE;
float g_freq = 1000.0f; 
int g_offset = 512;     // Midpoint default
float g_amp = 1.0f;     // Full scale 0-1023 (scaled by 511 * amp)
PIO g_pio = pio0;
uint g_sm = 0;
int g_dma_chan = -1;
int g_samples = BUF_LEN; // Number of active samples in the buffer for high speeds

void update_buffer() {
    float center = (float)g_offset;
    float scale = 511.0f * g_amp;

    printf("Updating buffer: Mode=%d, Offset=%d, Amp=%.2f, Samples=%d\n", g_mode, g_offset, g_amp, g_samples);

    for (int i = 0; i < g_samples; i++) {
        float v = 0.0f;
        float phase = (float)i / g_samples; // 0.0 to 1.0 based on Active Samples

        switch (g_mode) {
            case SINE:
                v = sin(phase * 2.0 * PI);
                break;
            case TRIANGLE:
                if (phase < 0.5) v = -1.0 + (phase * 4.0);
                else v = 1.0 - ((phase - 0.5) * 4.0);
                break;
            case SAWTOOTH:
                v = -1.0 + (phase * 2.0);
                break;
            case SQUARE:
                v = (phase < 0.5) ? 1.0f : -1.0f;
                break;
        }

        int16_t val = (int16_t)(center + (v * scale));
        
        // Clamp
        if (val < 0) val = 0;
        if (val > 1023) val = 1023;

        wave_buffer[i] = (uint16_t)val;
    }
}

void update_frequency() {
    float sys_clk = (float)(OVERCLOCK_KHZ * 1000);
    
    // The PIO loop takes 2 cycles per sample. rgco was able to fit it in one cycle, idk how bro
 
    float cycles_per_sample = 2.0f;

    // 1. Calculate ideal number of samples for MAXIMUM sample rate
    
    float ideal_samples = sys_clk / (g_freq * cycles_per_sample);
    int ring_bits = 0;
    
    if (ideal_samples >= 4096.0f) {
        g_samples = 4096;
        ring_bits = 13; 
    } else {
        // High Frequency Mode: Find nearest Power of 2 
        // At 384MHz, 4 samples * 2 cycles = 8 cycles total period = 48MHz max freq.
        int n = 4096;
        while (n > ideal_samples && n > 4) {
            n >>= 1;
        }
        g_samples = n;
        

        ring_bits = 0;
        int bytes = g_samples * 2;
        while ((1 << ring_bits) < bytes) ring_bits++;
    }
    

    float div = sys_clk / (g_freq * (float)g_samples * cycles_per_sample);

    if (div < 1.0f) div = 1.0f;
    if (div > 65535.0f) div = 65535.0f;

    // 3. Update PIO
    pio_sm_set_clkdiv(g_pio, g_sm, div);
    
    printf("Set Freq: %.2f Hz (Div: %.4f, Samples: %d, RingBits: %d)\n", g_freq, div, g_samples, ring_bits);
    
    // 4. Update buffer content for new sample count
    update_buffer();

    // 5. Reconfigure DMA for new buffer size
    if (g_dma_chan >= 0) {
        dma_channel_abort(g_dma_chan);
        
        dma_channel_config dc = dma_channel_get_default_config(g_dma_chan);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
        channel_config_set_read_increment(&dc, true);
        channel_config_set_dreq(&dc, pio_get_dreq(g_pio, g_sm, true));
        
        // Set Ring Buffer to new size
        channel_config_set_ring(&dc, false, ring_bits); 

        dma_channel_configure(
            g_dma_chan,
            &dc,
            &g_pio->txf[g_sm],
            wave_buffer,
            0xFFFFFFFF,
            true
        );
    }
}

void process_command(char* cmd) {
    char *token = strtok(cmd, " ");
    if (!token) return;

    if (strcmp(token, "sine") == 0) {
        g_mode = SINE;
        update_buffer();
    } else if (strcmp(token, "square") == 0) {
        g_mode = SQUARE;
        update_buffer();
    } else if (strcmp(token, "saw") == 0) {
        g_mode = SAWTOOTH;
        update_buffer();
    } else if (strcmp(token, "tri") == 0) {
        g_mode = TRIANGLE;
        update_buffer();
    } else if (strcmp(token, "freq") == 0) {
        token = strtok(NULL, " ");
        if (token) {
            g_freq = atof(token);
            update_frequency();
        }
    } else if (strcmp(token, "offset") == 0) {
        token = strtok(NULL, " ");
        if (token) {
            g_offset = atoi(token);
            update_buffer();
        }
    } else if (strcmp(token, "amp") == 0) {
        token = strtok(NULL, " ");
        if (token) {
            g_amp = atof(token);
            update_buffer();
        }
    } else {
        printf("Unknown command. Try: sine, square, saw, tri, freq <Hz>, offset <0-1023>, amp <0.0-1.0>\n");
    }
}

int main() {
    // Start with standard speeds first to ensure USB comes up
    stdio_init_all();
    sleep_ms(2000); 

    printf("\n\nRP2350 AWG Booting\n");

   
    // RP2350 Nominal is 150MHz.
    // Ensure we are explicitly set to expected speed for PIO math to be correct.
    vreg_set_voltage(VREG_VOLTAGE_1_30); 
    sleep_ms(10);
    if (set_sys_clock_khz(OVERCLOCK_KHZ, true)) {
        printf("Clock set to %d kHz\n", OVERCLOCK_KHZ);
    } else {
        printf("Failed to set clock speed!\n");
    }

  
    update_buffer();

  
    g_pio = pio0;
    g_sm = 0;
    uint offset = pio_add_program(g_pio, &dac_stream_program);

    pio_sm_config c = dac_stream_program_get_default_config(offset);
    
    sm_config_set_out_pins(&c, DAC_BASE_PIN, DAC_BITS);
    for(int i=0; i<DAC_BITS; i++) {
        pio_gpio_init(g_pio, DAC_BASE_PIN + i);
    }
    pio_sm_set_consecutive_pindirs(g_pio, g_sm, DAC_BASE_PIN, DAC_BITS, true);

    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    //  Autopull=True, Threshold=32  we consume full FIFO words
    // PIO handles discarding the extra bits manually
    sm_config_set_out_shift(&c, true, true, 32); 
    
    pio_sm_init(g_pio, g_sm, offset, &c);
    update_frequency(); // Sets the divider
    pio_sm_set_enabled(g_pio, g_sm, true);

 
    g_dma_chan = dma_claim_unused_channel(true);
    
 
    update_frequency();

    char line_buf[64];
    int char_idx = 0;

    printf("Ready. Type commands:\n");

    while (true) {
        int c_in = getchar_timeout_us(0);
        if (c_in != PICO_ERROR_TIMEOUT) {
            if (c_in == '\n' || c_in == '\r') {
                if (char_idx > 0) {
                    line_buf[char_idx] = 0;
                    process_command(line_buf);
                    char_idx = 0;
                }
            } else if (char_idx < 63) {
                line_buf[char_idx++] = (char)c_in;
            }
        }
 
    }
}