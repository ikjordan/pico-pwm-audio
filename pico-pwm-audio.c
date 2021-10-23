#include <stdio.h>
#include <math.h>          // For fminf and fmaxf
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/dma.h"  // dma 
#include "hardware/sync.h" // wait for interrupt 
#include "pico/util/queue.h" 

#include "pwm_channel.h"
#include "debounce_button.h"
#include "sound_buffers.h"
 
#define AUDIO_PIN 18  // Configured for the Maker board
#define STEREO        // When stereo enabled, currently DMA same data to both channels

/* 
 * This include brings in static arrays which contain audio samples. 
 * if you want to know how to make these please see the python code
 * for converting audio samples into static arrays. 
 */
//#include "preamble.h"
#include "panther.h"
//#include "ring.h"
//#include "sample.h"
//#include "thats_cool.h"

#ifdef STEREO
#define CHANNELS 2
#else
#define CHANNELS 1
#endif

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11000
#endif
#define DMA_BUFFER_LENGTH 2200      // 2200 samples @ 44kHz gives= 0.05 seconds = interrupt rate

#define RAM_BUFFER_LENGTH (2*DMA_BUFFER_LENGTH)
/*
 * Static variable definitions
 */
#ifdef TWELVE_BIT
static const int shift = 0;
#else
static const int shift = 3;
#endif

#define WRAP 4000                           // Maximum value for (reduced) 12 bit
#define MID_POINT (WRAP>>1)                 // Equates to mid point in (reduced) 12 bit

static int repeat_shift = 1;                // Defined by the sample rate
static int wav_position;                    // Holds position for left and right channels

static pwm_data pwm_channel[CHANNELS];   // Represents the PWM channels
static int dma_channel[2];

 // Have 2 buffers in RAM that are used to DMA the samples to the PWM engine
static uint32_t buffer[2][DMA_BUFFER_LENGTH];

// Have 2 or 4 8k buffers in RAM, copy data from Flash to these buffers - in future
// will be buffers where noise is created, or music delivered from SD Card

// RAM buffers - currently populated from Flash
static uint16_t ram_buffer[CHANNELS*2][RAM_BUFFER_LENGTH];

// Control data blocks for the RAM double buffers
static sound_buffers double_buffers[CHANNELS];

// Pointers to the currenly in use RAM buffers - one pointer for left and (optionally) one for right
static const uint16_t* current_RAM_Buffer[CHANNELS];

static float volume = 0.4;                  // Volume adjust, will be controlled by button

queue_t eventQueue;

enum Event 
{
    empty = 0,
    increase = 1, 
    decrease = 2,
    fill_queue = 3, 
    quit = 4, 
}; 

static debounce_button_data button[4];
/* 
 * Function declarations
 */
static void populateDmaBuffer(int buffer_index);
static void claimDmaChannels(int num_channels);
static void initDma(int buffer_index, int slice, int chain_index);
static void dmaInterruptHandler();
int getRepeatShift(uint sample_rate);

void stopMusic();
void buttonCallback(uint gpio_number, enum debounce_event event);

/* 
 * Function definitions
 */

// Handles interrupts for the DMA chain
// Loads next buffer, and ressts start address for DMA
static void dmaInterruptHandler() 
{
    // Determine which DMA caused the interrupt
    for (int i = 0 ; i<2; ++i)
    {
        if (dma_channel_get_irq0_status(dma_channel[i]))
        {
            dma_channel_acknowledge_irq0(dma_channel[i]);
            populateDmaBuffer(i);
            dma_channel_set_read_addr(dma_channel[i], buffer[i], false);
        }
    }    
}

// Populate the DMA buffer, referenced by index
static void populateDmaBuffer(int buffer_index)
{
    // Populate two bytes from each active buffer
    for (int i=0; i<DMA_BUFFER_LENGTH; ++i)
    {
        // Write to buffer, adjusting for volume
        // build the 32 bit word from the two channels
        uint32_t left = (((current_RAM_Buffer[0][wav_position>>repeat_shift] << shift) - MID_POINT) * volume) + MID_POINT;
        uint32_t right = MID_POINT;

        if (CHANNELS == 2)
        {
            right = (((current_RAM_Buffer[1][wav_position>>repeat_shift] << shift) - MID_POINT) * volume) + MID_POINT;
        }

        buffer[buffer_index][i] = (left << 16) + right;

        if (wav_position < (RAM_BUFFER_LENGTH<<repeat_shift) - 1) 
        { 
            wav_position++;
        } else 
        {
            // We need a new RAM buffer
            current_RAM_Buffer[0] = soundBufferGetLast(&double_buffers[0]);
            if (CHANNELS == 2)            
            {
                current_RAM_Buffer[1] = soundBufferGetLast(&double_buffers[1]);
            }
            // reset to start
            wav_position = 0;

            // Signal to populate a new RAM buffer
            enum Event e = fill_queue;
            queue_try_add(&eventQueue, &e);
        }
    }
}

// Obtain the DMA channels - need 2 
static void claimDmaChannels(int num_channels)
{
    for (int i=0; i<num_channels; ++i)
    {
        dma_channel[i] = dma_claim_unused_channel(true); 
    }
}

// Configure the DMA channels - including chaining
static void initDma(int buffer_index, int slice, int chain_index)
{
    dma_channel_config config = dma_channel_get_default_config(dma_channel[buffer_index]); 
    channel_config_set_read_increment(&config, true); 
    channel_config_set_write_increment(&config, false); 
    channel_config_set_dreq(&config, DREQ_PWM_WRAP0 + slice); 
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32); 
    channel_config_set_chain_to(&config, dma_channel[chain_index]);

    // Set up config
    dma_channel_configure(dma_channel[buffer_index], 
                          &config, 
                          &pwm_hw->slice[slice].cc, 
                          buffer[buffer_index],
                          DMA_BUFFER_LENGTH,
                          false);
}

// Determine how often a value needs to be repeated, based on the sampling rate
// If repeat is 2^m return m
int getRepeatShift(uint sample_rate)
{
    int ret;
    // Determine the repeat rate
    switch (sample_rate)
    {
        case 11000:
            ret = 2;
        break;

        case 22000:
            ret = 1;
        break;

        case 44000:
            ret = 0;
        break;

        default:
            // Not a supported rate
            ret = -1;
        break;
    }
    return ret;
}

int main(void) 
{
    /* Overclocking for fun but then also so the system clock is a 
     * multiple of typical audio sampling rates.
     */
    stdio_init_all();

    // Overclock to 176MHz - assert if cannot be set
    set_sys_clock_khz(176000, true);   
    
    // Initialise the repeat shift, based on sampling rate
    repeat_shift = getRepeatShift(SAMPLE_RATE);

    // Set up the PWMs - A single pwm period will be 176 MHz / WRAP = 44 kHz
    pwmChannelInit(&pwm_channel[0], AUDIO_PIN, 1.0f, WRAP);

    // Set the initial value of the pwm before start
    pwmChannelSetFirstValue(&pwm_channel[0], MID_POINT);

    // Set up the right channel, if in stereo
    if(CHANNELS == 2)
    {
        pwmChannelInit(&pwm_channel[1], AUDIO_PIN+1, 1.0f, WRAP);

        // Set the initial value of the pwm before start
        pwmChannelSetFirstValue(&pwm_channel[1], MID_POINT);
    }

    // Initialise the DMA(s)
    claimDmaChannels(2);

    // Initialise and Chain the two DMAs together
    initDma(0, pwmChannelGetSlice(&pwm_channel[0]), 1);
    initDma(1, pwmChannelGetSlice(&pwm_channel[0]), 0);

    // Set the DMA interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_0, dmaInterruptHandler); 

    // Enable the interrupts, only handle interrupts on left channel
    int mask = 0;

    for (int i=0;i<2;++i)
    {
        mask |= 0x01 << dma_channel[i];
    }

    dma_set_irq0_channel_mask_enabled(mask, true);
    irq_set_enabled(DMA_IRQ_0, true);

    // Build the pwm start mask
    uint32_t pwm_mask = 0;
    
    pwmChannelAddStartList(&pwm_channel[0], &pwm_mask);

    if (CHANNELS == 2)
    {
        // Build the right start mask
        pwmChannelAddStartList(&pwm_channel[1], &pwm_mask);
    }
    
    // Build the DMA start mask
    uint32_t chan_mask = 0x01 << dma_channel[0];

    // Initialise the buttons
    debounceButtonCreate(&button[0], 20, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[1], 21, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[2], 22, 40, buttonCallback, true, false);
    //debounceButtonCreate(&button[3], 14, 40, buttonCallback, false, true);

    // Create the event queue
    enum Event event = empty;
    queue_init(&eventQueue, sizeof(event), 4);

    /*
     * Pre-Populate the buffers
     */
    // Set up and create the sound double buffers in RAM
    current_RAM_Buffer[0] = soundBuffersCreate(&double_buffers[0], ram_buffer[0], ram_buffer[1],RAM_BUFFER_LENGTH, WAV_DATA, WAV_DATA_LENGTH);

    if (CHANNELS == 2)
    {
        current_RAM_Buffer[1] = soundBuffersCreate(&double_buffers[1], ram_buffer[2], ram_buffer[3], RAM_BUFFER_LENGTH, WAV_DATA, WAV_DATA_LENGTH);
    }

    // Populate the DMA buffers
    populateDmaBuffer(0);
    populateDmaBuffer(1);

    // Main loop - would generate noise, handle buttons for volume, parse wav blocks etc here 
    // set one shot timer to stop after 40 seconds
    //add_alarm_in_ms(4000, alarm_callback, NULL, false);

    // Start the DMA and PWM
    dma_start_channel_mask(chan_mask);
    pwmChannelStartList(pwm_mask);

    // Process events
    bool cont = true;
    while (cont)
    {
        queue_remove_blocking(&eventQueue, &event);
        
        switch (event)
        {
            case increase:
                volume = fminf(1.0, volume+0.1);
            break;

            case decrease:
                volume = fmaxf(0.0, volume-0.1);
            break;

            case fill_queue:
                soundBuffersPopulateNext(&double_buffers[0]);

                if (CHANNELS == 2)
                {
                    soundBuffersPopulateNext(&double_buffers[1]);
                }
            break;

            case quit:
                cont = false;
            break;

            default:
                return -1;
            break;
        }
    }
    stopMusic();

    return 0;
}

void buttonCallback(uint gpio_number, enum debounce_event event)
{
    enum Event e = empty;

    switch (gpio_number)
    {
        case 14:
            e = increase;
        break;

        case 20:
            e = increase;
        break;

        case 21:
            e = decrease;
        break;

        case 22:
            e = quit;
        break;
    }
    queue_try_add(&eventQueue, &e);
}

void stopMusic()
{
        
    for (int i=0; i<(CHANNELS); ++i)
    {
        pwmChannelStop(&pwm_channel[i]);
    }

    for (int i=0; i<2; ++i)
    {
        dma_channel_abort(dma_channel[i]);
    }
}

