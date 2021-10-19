#include <stdio.h>
#include <math.h>          // For fminf and fmaxf
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/dma.h"  // dma 
#include "hardware/sync.h" // wait for interrupt 
#include "pico/util/queue.h" 

#include "pwm_channel.h"
 
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
#define CHANNELS 4
#else
#define CHANNELS 2
#endif

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11000
#endif
#define DMA_BUFFER_LENGTH 2200      // 2200 samples @ 44kHz gives= 0.05 seconds = interrupt rate
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
static int wav_position[CHANNELS>>1];       // Holds position for left and right channels

static pwm_data pwm_channel[CHANNELS>>1];   // Represents the PWM channels

 // Have 2 or 4 1.1 kBytes buffers in RAM that are used to DMA the samples to the PWM engine
static uint16_t buffer[CHANNELS][DMA_BUFFER_LENGTH];
static int dma_channel[CHANNELS];

static float volume = 0.1;                  // Volume adjust, will be controlled by button

queue_t eventQueue;

enum Event 
{
    empty = 0,
    increase = 1, 
    decrease = 2, 
    quit = 3, 
}; 

/* 
 * Function declarations
 */
static void populateDmaBuffer(int buffer_index);
static void claimDmaChannels(int num_channels);
static void initDma(int buffer_index, int slice, int chain_index);
static void dmaInterruptHandler();
int getRepeatShift(uint sample_rate);

void stopMusic();
int64_t alarm_callback(alarm_id_t id, void *user_data);
bool repeating_timer_callback(struct repeating_timer *t);
/* 
 * Function definitions
 */

// Handles interrupts for the left channel (only one linked to IRQ)
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

            if (CHANNELS > 2)
            {
                populateDmaBuffer(i+2);
                dma_channel_set_read_addr(dma_channel[i+2], buffer[i+2], false);
            }
        }
    }    
}

// Populate the DMA buffer, referenced by index
static void populateDmaBuffer(int buffer_index)
{
    for (int i=0; i<DMA_BUFFER_LENGTH; ++i)
    {
        // Write to buffer, adjusting for volume
        buffer[buffer_index][i] = (((WAV_DATA[wav_position[buffer_index>>1]>>repeat_shift] << shift) - MID_POINT) * volume) + MID_POINT;

        if (wav_position[buffer_index>>1] < (WAV_DATA_LENGTH<<repeat_shift) - 1) 
        { 
            wav_position[buffer_index>>1]++;
        } else 
        {
            // reset to start
            wav_position[buffer_index>>1] = 0;
        }
    }
}

// Obtain the DMA channels - need 2 per channel (i.e. 4 for stereo)
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
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16); 
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
    for (int i=0; i<(CHANNELS>>1); ++i)
    {
        pwmChannelInit(&pwm_channel[i], AUDIO_PIN+i, 1.0f, WRAP);

        // Set the initial value of the pwm before start
        pwmChannelSetFirstValue(&pwm_channel[i], MID_POINT);

    }

    // Initialise the DMA(s)
    claimDmaChannels(CHANNELS);

    for (int i=0; i<CHANNELS; i+=2)
    {
        initDma(i, pwmChannelGetSlice(&pwm_channel[i>>1]), i+1);
        initDma(i+1, pwmChannelGetSlice(&pwm_channel[i>>1]), i);
    }
    // Populate the buffers
    for (int i=0; i<CHANNELS; ++i)
    {
        populateDmaBuffer(i);
    }

    // Set the DMA interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_0, dmaInterruptHandler); 

    // Enable the interrupts, only handle interrupts on left channel
    int mask = 0;
    for (int i=0;i<2;++i)
    {
        mask |= 0x01 << dma_channel[i];
    }
    dma_set_irq0_channel_mask_enabled(mask, true);

    // enable the interrupts
    irq_set_enabled(DMA_IRQ_0, true);

    // Trigger the DMA(s)
    uint32_t chan_mask = 0;
    
    for (int i=0; i<CHANNELS; i+=2)
    {
        chan_mask |= 0x01 << dma_channel[i];
    }

    // Start the PWMs
    uint32_t pwm_mask = 0;
    for (int i=0; i<(CHANNELS>>1); ++i)
    {
        // Build the start mask
        pwmChannelAddStartList(&pwm_channel[i], &pwm_mask);
    }
    
    dma_start_channel_mask(chan_mask);
    pwmChannelStartList(pwm_mask);

    // Main loop - would generate noise, handle buttons for volume, parse wav blocks etc here 
    // set one shot timer to stop after 40 seconds
    //add_alarm_in_ms(4000, alarm_callback, NULL, false);
    enum Event event = empty;

    // Create event queue 
    queue_init(&eventQueue, sizeof(event), 4);

    // Set the alarm to increase volume
    repeating_timer_t repeat;
    add_repeating_timer_ms(4000, repeating_timer_callback, NULL, &repeat);

    // Set the alarm to stop
    add_alarm_in_ms(41000, alarm_callback, NULL, true);

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

            case quit:
                cont = false;
            break;

            default:
                return -1;
            break;
        }
    }
    cancel_repeating_timer(&repeat);
    stopMusic();

    return 0;
}

bool repeating_timer_callback(struct repeating_timer *t) 
{
    enum Event e = increase;
    
    queue_try_add(&eventQueue, &e);
    return true;
}

int64_t alarm_callback(alarm_id_t id, void *user_data) 
{
    enum Event e = quit;
    queue_try_add(&eventQueue, &e);
    return 0;
}

void stopMusic()
{
        
    for (int i=0; i<(CHANNELS>>1); ++i)
    {
        pwmChannelStop(&pwm_channel[i]);
    }

    for (int i=0; i<CHANNELS; ++i)
    {
        dma_channel_abort(dma_channel[i]);
    }
}

