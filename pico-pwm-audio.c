#include <stdio.h>
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/pwm.h"  // pwm 
#include "hardware/dma.h"  // dma 
#include "hardware/sync.h" // wait for interrupt 
 
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
#define DMA_BUFFER_LENGTH 550      // 550 samples = 0.05 seconds at 11 KHz, so volume will change on avaerge in 0.15 seconds
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
static int audio_pin_slice[CHANNELS>>1];    // PWM slide for left and right channels

 // Have 2 or 4 1.1 kBytes buffers in RAM that are used to DMA the samples to the PWM engine
static volatile uint16_t buffer[CHANNELS][DMA_BUFFER_LENGTH];
static int dma_channel[CHANNELS];

static float volume = 0.1;                  // Volume adjust, will be controlled by button

/* 
 * Function declarations
 */
static void populate_dma_buffer(int buffer_index);
static void claim_dma_channels(int num_channels);
static void initialise_dma(int buffer_index, int slice, int chain_index);
static void dma_interrupt_handler();

/* 
 * Function definitions
 */

// Handles interrupts for the left channel (only one linked to IRQ)
// Loads next buffer, and ressts start address for DMA
static void dma_interrupt_handler() 
{
    // Determine which DMA caused the interrupt
    for (int i = 0 ; i<2; ++i)
    {
        if (dma_channel_get_irq0_status(dma_channel[i]))
        {
            dma_channel_acknowledge_irq0(dma_channel[i]);
            populate_dma_buffer(i);
            dma_channel_set_read_addr(dma_channel[i], buffer[i], false);

            if (CHANNELS > 2)
            {
                populate_dma_buffer(i+2);
                dma_channel_set_read_addr(dma_channel[i+2], buffer[i+2], false);
            }
        }
    }    
}

// Populate the DMA buffer, referenced by index
static void populate_dma_buffer(int buffer_index)
{
    for (int i=0; i<DMA_BUFFER_LENGTH; ++i)
    {
        if (wav_position[buffer_index>>1] < (WAV_DATA_LENGTH<<repeat_shift) - 1) 
        { 
            wav_position[buffer_index>>1]++;
        } else 
        {
            // reset to start
            wav_position[buffer_index>>1] = 0;
        }
        // Write to buffer, adjusting for volume
        buffer[buffer_index][i] = (((WAV_DATA[wav_position[buffer_index>>1]>>repeat_shift] << shift) - MID_POINT) * volume) + MID_POINT;
    }
}

// Obtain the DMA channels - need 2 per channel (i.e. 4 for stereo)
static void claim_dma_channels(int num_channels)
{
    for (int i=0; i<num_channels; ++i)
    {
        dma_channel[i] = dma_claim_unused_channel(true); 
    }
}

// Configure the DMA channels - including chaining
static void initialise_dma(int buffer_index, int slice, int chain_index)
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

int main(void) 
{
    /* Overclocking for fun but then also so the system clock is a 
     * multiple of typical audio sampling rates.
     */
    stdio_init_all();

    // Overclock to 176MHz - assert if cannot be set
    set_sys_clock_khz(176000, true);   

    for (int i=0; i<(CHANNELS>>1); ++i)
    {
        // Set the audio pin to PWM
        gpio_set_function(AUDIO_PIN+i, GPIO_FUNC_PWM); 
        // Get which of the 8 pwm slices has been assigned to the PWM
        audio_pin_slice[i] = pwm_gpio_to_slice_num(AUDIO_PIN+i);
    }

    // Setup PWM for audio output
    pwm_config config = pwm_get_default_config();
    /* Base clock 176,000,000 Hz 
       Want 12 bit samples at 44kHz - 4000 * 44kHz gives 176,000,000
     * 
     * For lower sample rates - repeat the sample
     *  4 repeats for 11 KHz
     *  2 repeats for 22 KHz
     */
    // Note we can only support range 0-3999, rather than full 12 bit range, this is due to 
    // issues setting the pico clock to exactly the right speed

    // We want to use the full clock range - so set the divider to 1
    pwm_config_set_clkdiv(&config, 1.0f); 

    // Set the wrap to 4000, giving us our (slightly reduced) 12 bits at 44 kHz
    // For CD quality it should really be 44.1 kHz...
    pwm_config_set_wrap(&config, WRAP); 

    // Determine the repeat rate
    switch (SAMPLE_RATE)
    {
        case 11000:
            repeat_shift = 2;
        break;

        case 22000:
            repeat_shift = 1;
        break;

        case 44000:
            repeat_shift = 0;
        break;

        default:
            // Not a supported rate
            return -1;
        break;
    }

    // Initialise the DMA(s)
    claim_dma_channels(CHANNELS);

    for (int i=0; i<CHANNELS; i+=2)
    {
        initialise_dma(i, audio_pin_slice[i>>1], i+1);
        initialise_dma(i+1, audio_pin_slice[i>>1], i);
    }
    // Populate the buffers
    for (int i=0; i<CHANNELS; ++i)
    {
        populate_dma_buffer(i);
    }

    // Set the DMA interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_0, dma_interrupt_handler); 

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
    for (int i=0; i<CHANNELS; i+=2)
    {
        dma_channel_start(dma_channel[i]);
    }

    // Complete configuration of the PWMs and start them
    for (int i=0; i<(CHANNELS>>1); ++i)
    {
        // Set the initial value of the pwm before start
        pwm_set_chan_level(audio_pin_slice[i], pwm_gpio_to_channel(AUDIO_PIN+i), WAV_DATA[0]);
        pwm_init(audio_pin_slice[i], &config, true);
    }

    // Main loop - would generate noise, handle buttons for volume, parse wav blocks etc here 
    while(1) 
    {
        __wfi(); // Wait for Interrupt
    }
}
