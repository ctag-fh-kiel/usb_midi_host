/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 rppicomidi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/**
 * This demo program is designed to test the USB MIDI Host driver for a single USB
 * MIDI device connected to the USB Host port. It sends to the USB MIDI device the
 * sequence of half-steps from B-flat to D whose note numbers correspond to the
 * transport button LEDs on a Mackie Control compatible control surface. It also
 * prints to a UART serial port console the messages received from the USB MIDI device.
 *
 * This program works with a single USB MIDI device connected via a USB hub, but it
 * does not handle multiple USB MIDI devices connected at the same time.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "bsp/board_api.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "usb_midi_host.h"


// On-board LED mapping. If no LED, set to NO_LED_GPIO
const uint NO_LED_GPIO = 255;
const uint LED_GPIO = 25;
#define MCU_GPIO_SEL 1
#define WS_PIN 5
#define SPI_PORT spi1
#define SPI_SPEED 8000000
#define SPI_SCLK 26
#define SPI_MOSI 27
#define SPI_MISO 28
#define SPI_CS 29
#define SPI_BUFFER_LEN 64

static uint8_t midi_dev_addr = 0;

semaphore_t ws_semaphore;
mutex_t data_mutex;

// transfer structure is
// byte 0, 1 -> 0xCA 0xFE (fingerprint)
// byte 2 -> amount of data bytes (max. 64 - 3)
uint8_t out_buf[SPI_BUFFER_LEN], in_buf[SPI_BUFFER_LEN];

static void blink_led(void)
{
    static absolute_time_t previous_timestamp = {0};

    static bool led_state = false;

    // This design has no on-board LED
    if (NO_LED_GPIO == LED_GPIO)
        return;
    absolute_time_t now = get_absolute_time();
    
    int64_t diff = absolute_time_diff_us(previous_timestamp, now);
    if (diff > 1000000) {
        gpio_put(LED_GPIO, led_state);
        led_state = !led_state;
        previous_timestamp = now;
    }
}

static void send_next_note(bool connected)
{
    static uint8_t first_note = 0x5b; // Mackie Control rewind
    static uint8_t last_note = 0x5f; // Mackie Control stop
    static uint8_t message[6] = {
        0x90, 0x5f, 0x00,
        0x90, 0x5b, 0x7f,
    };
    // toggle NOTE On, Note Off for the Mackie Control channels 1-8 REC LED
    const uint32_t interval_ms = 1000;
    static uint32_t start_ms = 0;

    // device must be attached and have at least one endpoint ready to receive a message
    if (!connected || tuh_midih_get_num_tx_cables(midi_dev_addr) < 1)
        return;

    // transmit any previously queued bytes
    tuh_midi_stream_flush(midi_dev_addr);
    // Blink every interval ms
    if ( board_millis() - start_ms < interval_ms) {
        return; // not enough time
    }
    start_ms += interval_ms;

    uint32_t nwritten = 0;
    // Transmit the note message on the highest cable number
    uint8_t cable = tuh_midih_get_num_tx_cables(midi_dev_addr) - 1;
    nwritten = 0;
    nwritten += tuh_midi_stream_write(midi_dev_addr, cable, message, sizeof(message));
 
    if (nwritten != 0)
    {
        ++message[1];
        ++message[4];
        if (message[1] > last_note)
            message[1] = first_note;
        if (message[4] > last_note)
            message[4] = first_note;
    }
    tuh_midi_stream_flush(midi_dev_addr);
}

static void ws_callback(uint gpio, uint32_t events) {
    // sync to word clock of codec i2s @ 44100Hz
    // divider 32 is block size of TBD
    static int div = 32;
    div--;
    if(div <= 0){
        div = 32;
        sem_release(&ws_semaphore);
    }
}

void core1_entry() {

    multicore_fifo_push_blocking(42);

    uint32_t g = multicore_fifo_pop_blocking();

    if (g != 43)
        printf("Hmm, that's not right on core 1!\n");
    else
        printf("Its all gone well on core 1!");

    printf("SPI setup\n");

    // Enable SPI 0 at 1 MHz and connect to GPIOs
    spi_init(SPI_PORT, SPI_SPEED);
    gpio_set_function(SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MOSI, GPIO_FUNC_SPI);
    //gpio_set_function(SPI_CS, GPIO_FUNC_SPI);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Configure CS pin
    gpio_init(SPI_CS);
    gpio_set_dir(SPI_CS, GPIO_OUT);
    gpio_put(SPI_CS, 1); // Deassert CS initially


    // Make the SPI pins available to picotool
    //bi_decl(bi_4pins_with_func(PICO_DEFAULT_SPI_RX_PIN, PICO_DEFAULT_SPI_TX_PIN, PICO_DEFAULT_SPI_SCK_PIN, PICO_DEFAULT_SPI_CSN_PIN, GPIO_FUNC_SPI));
    bi_decl(bi_3pins_with_func(PICO_DEFAULT_SPI_RX_PIN, PICO_DEFAULT_SPI_TX_PIN, PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI));


    // initialize semaphore
    sem_init(&ws_semaphore, 0, 1);

    // enable GPIO interrupts
    // set IRQ for WS pin
    gpio_set_irq_enabled_with_callback(WS_PIN, GPIO_IRQ_EDGE_FALL, true, &ws_callback);

    printf("Starting core1 event loop\n");
    out_buf[0] = 0xCA;
    out_buf[1] = 0xFE;
    while (1){
        uint8_t len;
        // TODO do this with DMA
        sem_acquire_blocking(&ws_semaphore);
        // Lock the mutex
        mutex_enter_blocking(&data_mutex);
        // transfer data
        gpio_put(SPI_CS, 0);
        len = spi_write_read_blocking(SPI_PORT, out_buf, in_buf, SPI_BUFFER_LEN);
        gpio_put(SPI_CS, 1);
        out_buf[2] = 0x00;
        // Unlock the mutex
        mutex_exit(&data_mutex);
    }
        //tight_loop_contents();
}

int main() {

    bi_decl(bi_program_description("A USB MIDI host example."));
    bi_decl(bi_1pin_with_name(LED_GPIO, "On-board LED"));

    board_init();
    printf("Pico MIDI Host Example\r\n");

    // initialize data mutex
    printf("Initializing data mutex\n");
    mutex_init(&data_mutex);


    // Enable USB-A
    gpio_init(MCU_GPIO_SEL);
    gpio_set_dir(MCU_GPIO_SEL, GPIO_OUT);
    gpio_put(MCU_GPIO_SEL, 1);
    tusb_init();

    // Map the pins to functions
    gpio_init(LED_GPIO);
    gpio_set_dir(LED_GPIO, GPIO_OUT);

    // start multicore
    multicore_launch_core1(core1_entry);

    // Wait for it to start up
    uint32_t g = multicore_fifo_pop_blocking();

    if (g != 42)
        printf("Hmm, that's not right on core 0!\n");
    else {
        multicore_fifo_push_blocking(43);
        printf("It's all gone well on core 0!");
    }

    printf("Starting core0 event loop\n");
    while (1) {
        tuh_task();

        blink_led();
        bool connected = midi_dev_addr != 0 && tuh_midi_configured(midi_dev_addr);

        send_next_note(connected);
    }
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx, uint16_t num_cables_tx)
{
  printf("MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u cables\r\n",
      dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

  if (midi_dev_addr == 0) {
    // then no MIDI device is currently connected
    midi_dev_addr = dev_addr;
  }
  else {
    printf("A different USB MIDI Device is already connected.\r\nOnly one device at a time is supported in this program\r\nDevice is disabled\r\n");
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  if (dev_addr == midi_dev_addr) {
    midi_dev_addr = 0;
    printf("MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
  }
  else {
    printf("Unused MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
  }
}

void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets)
{
    printf("Midi Callback\r\n");
  if (midi_dev_addr == dev_addr) {
    if (num_packets != 0) {
      uint8_t cable_num;
      uint8_t buffer[48];
      while (1) {
        uint32_t bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, sizeof(buffer));
        if (bytes_read == 0) return;

        // put data into buffer for SPI transfer
        // TODO split messages if bytes_read > SPI_BUFFER_LEN - 3
        if(bytes_read > SPI_BUFFER_LEN - 3){
          printf("MIDI RX Cable #%u: Message too long\r\n", cable_num);
        }else{
            // Lock the mutex
            mutex_enter_blocking(&data_mutex);
            // copy data to be transferred
            out_buf[2] = (uint8_t) bytes_read;
            memcpy(out_buf + 3, buffer, bytes_read);
            // Unlock the mutex
            mutex_exit(&data_mutex);
        }
        printf("MIDI RX Cable #%u, bytes read %d, values:", cable_num, bytes_read);
        for (uint32_t idx = 0; idx < bytes_read; idx++) {
          printf("%02x ", buffer[idx]);
        }
        printf("\r\n");
      }
    }
  }
}

void tuh_midi_tx_cb(uint8_t dev_addr)
{
    (void)dev_addr;
}
