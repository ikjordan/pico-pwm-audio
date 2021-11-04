#include <stdio.h>
#include <math.h>          // For fminf and fmaxf
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/dma.h"  // dma 
#include "hardware/sync.h" // wait for interrupt 
#include "pico/util/queue.h" 
#include "rtc.h"
#include "f_util.h"
#include "ff.h"
#include "hw_config.h"

#include "pwm_channel.h"
#include "debounce_button.h"
#include "double_buffer.h"
#include "circular_buffer.h"
#include "colour_noise.h"
#include "wave_file.h"

 
#define AUDIO_PIN 18  // Configured for the Maker board 18 left, 19 right
#define STEREO        // When stereo enabled, currently DMA same data to both channels
//#define NOISE

/* 
 * This include brings in static arrays which contain audio samples. 
 * if you want to know how to make these please see the python code
 * for converting audio samples into static arrays. 
 */
//#include "preamble.h"
//#include "panther.h"
#include "ring.h"
//#include "sample.h"
//#include "thats_cool.h"

#ifdef STEREO
bool stereo = true;
#else
bool stereo = false;
#endif

colour_noise cn[2];
void createNoise(uint16_t* buffer, uint len);   // Call back to generate next buffer of noise

static circular_buffer sb;
void getSound(uint16_t* buffer, uint len);      // Call back to populate buffer from cicrular buffer
void getFileSound(uint16_t* buffer, uint len);  // Call back to populate buffer from file

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11000
#endif
#define DMA_BUFFER_LENGTH 2200      // 2200 samples @ 44kHz gives= 0.05 seconds = interrupt rate

#define RAM_BUFFER_LENGTH (4*DMA_BUFFER_LENGTH)
/*
 * Static variable definitions
 */
#ifdef TWELVE_BIT
static const int shift = 0;
#else
static const int shift = 3;
#endif

static uint wrap;                           // Largest value a sample can be + 1
static int mid_point;                       // wrap divided by 2
static float fraction = 1;                  // Divider used for PWM
static int repeat_shift = 1;                // Defined by the sample rate
static int wav_position;                    // Holds current position in ram_buffers for channels

static pwm_data pwm_channel[2];             // Represents the PWM channels
static int dma_channel[2];                  // The 2 DMA channels used for DMA ping pong
static int dma_buffer_index = 0;            // Index into active DMA buffer

 // Have 2 buffers in RAM that are used to DMA the samples to the PWM engine
static uint32_t dma_buffer[2][DMA_BUFFER_LENGTH];

// Have 2 or 4 8k buffers in RAM, copy data from Flash to these buffers - in future
// will be buffers where noise is created, or music delivered from SD Card

// RAM buffers, controlled through double_buffer class
static uint16_t ram_buffer[2][RAM_BUFFER_LENGTH];

// Control data blocks for the RAM double buffers
static double_buffer double_buffers;

static wave_file wf;
static sd_card_t* pSD;
static FIL fil;

// Pointer to the currenly in use RAM buffer
static const uint16_t* current_RAM_Buffer;

static float volume = 0.8;                  // Initial volume adjust, controlled by button

// Event queue, used to leave ISR context
static queue_t eventQueue;

// Collection of events we support
enum Event 
{
    empty = 0,
    increase = empty + 1, 
    decrease = increase + 1,
    populate_dma = decrease + 1,
    populate_double = populate_dma + 1,
    change = populate_double + 1,
    quit = change + 1, 
}; 

// Range of sound colours and files we can play
enum sound_colour
{
    white = 0,
    pink = white + 1,
    brown = pink + 1,
    file_1 = brown + 1,
    file_2 = file_1 + 1,
    max = file_2
};

// Start with brownian (red) noise
enum sound_colour colour = brown;

// Four buttons
static debounce_button_data button[4];

/* 
 * Function declarations
 */
static void populateDmaBuffer(void);
static void claimDmaChannels(int num_channels);
static void initDma(int buffer_index, int slice, int chain_index);
static void dmaInterruptHandler();
static bool getRepeatShift(uint sample_rate, uint* shift, uint* wrap, uint* mid_point, float* fraction);

void stopMusic();
void pauseMusic();
void buttonCallback(uint gpio_number, enum debounce_event event);

static bool attemptFile(const char* filename);
static bool mount(void);
static void unmount(void);
static bool mounted = false;

/* 
 * Function definitions
 */

// Handles interrupts for the DMA chain
// Resets start address for DMA and requests buffer that is exhausted to be refilled
static void dmaInterruptHandler() 
{
    // Determine which DMA caused the interrupt
    for (int i = 0 ; i<2; ++i)
    {
        if (dma_channel_get_irq1_status(dma_channel[i]))
        {
            dma_channel_acknowledge_irq1(dma_channel[i]);
            dma_channel_set_read_addr(dma_channel[i], dma_buffer[i], false);

            // Populate buffer outside of IRQ
            enum Event e = populate_dma;
            queue_try_add(&eventQueue, &e);
        }
    }    
}

// Populate the DMA buffer, referenced by index
static void populateDmaBuffer(void)
{
    // Populate two bytes from each active buffer
    for (int i=0; i<DMA_BUFFER_LENGTH; ++i)
    {
        // Write to buffer, adjusting for volume
        // build the 32 bit word from the two channels
        uint32_t left = ((current_RAM_Buffer[(wav_position>>repeat_shift)<<1]) - mid_point) * volume + mid_point;
        uint32_t right = ((current_RAM_Buffer[((wav_position>>repeat_shift)<<1)+1]) - mid_point) * volume + mid_point;
        wav_position++;

        if (!stereo)
        {
            // Want mono, so average two channels
            left = (left + right) >> 1;
            right = left;
        }

        // Combine the two channels
        dma_buffer[dma_buffer_index][i] = (left << 16) + right;

        if ((wav_position<<1) == (RAM_BUFFER_LENGTH<<repeat_shift)) 
        {
            // Need a new RAM buffer
            current_RAM_Buffer = doubleBufferGetLast(&double_buffers);

            // reset to start
            wav_position = 0;

            // Signal to populate a new RAM buffer
            enum Event e = populate_double;
            queue_try_add(&eventQueue, &e);
        }
    }
    dma_buffer_index = 1 - dma_buffer_index;
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
                          dma_buffer[buffer_index],
                          DMA_BUFFER_LENGTH,
                          false);
}

// Determine how often a value needs to be repeated, based on the sampling rate
// If repeat is 2^m return m
static bool getRepeatShift(uint sample_rate, uint* shift, uint* wrap, uint* mid_point, float* fraction)
{
    bool ret = true;
    // Determine the repeat rate
    switch (sample_rate)
    {
        case 11000:
            *shift = 2;
            *wrap = 4091;
            *fraction = 1.0f;
        break;

        case 22000:
            *shift = 1;
            *wrap = 4091;
            *fraction = 1.0f;
        break;

        case 22050:
            *shift = 1;
            *wrap = 4082;
            *fraction = 1.0f;
        break;

        case 44000:
            *shift = 0;
            *wrap = 4091;
            *fraction = 1.0f;
        break;

        case 44100:
            *shift = 0;
            *wrap = 4082;
            *fraction = 1.0f;
        break;

        case 8000:
            *shift = 2;
            *wrap = 4091;
            *fraction = 1.375f;
        break;

        case 16000:
            *shift = 1;
            *wrap = 4091;
            *fraction = 1.375f;
        break;

        case 32000:
            *shift = 0;
            *wrap = 4091;
            *fraction = 1.375f;
        break;

        default:
            // Not a supported rate
            *wrap = 0;
            *mid_point = *wrap >> 1;
            ret = false;
        break;
    }
    return ret;
}

int main(void) 
{
    // Overclock to 176MHz so that system clock is a multiple of typical
    // audio sampling rates - assert if cannot be set
    set_sys_clock_khz(180000, true);   
    
    // Adjust clock before initialiing, so serial port will work
    stdio_init_all();
    time_init();

    // Initialise the repeat shift, based on sampling rate
    getRepeatShift(SAMPLE_RATE, &repeat_shift, &wrap, &mid_point, &fraction);

    // Attempt to mount the file system
    mount();

    if (mounted)
    {
        printf("Mounted\n");
    }
    else
    {
        printf("Mount failed");
    }

    // Set up the PWMs
    pwmChannelInit(&pwm_channel[0], AUDIO_PIN, fraction, wrap);

    // Set the initial value of the pwm before start
    pwmChannelSetFirstValue(&pwm_channel[0], mid_point);

    // Set up the right channel
    pwmChannelInit(&pwm_channel[1], AUDIO_PIN+1, fraction, wrap);

    // Set the initial value of the pwm before start
    pwmChannelSetFirstValue(&pwm_channel[1], mid_point);

    // Initialise the DMA(s)
    claimDmaChannels(2);

    // Initialise and Chain the two DMAs together
    initDma(0, pwmChannelGetSlice(&pwm_channel[0]), 1);
    initDma(1, pwmChannelGetSlice(&pwm_channel[0]), 0);

    // Set the DMA interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_1, dmaInterruptHandler); 

    // Enable the interrupts for both of the chained dma channels
    int mask = 0;

    for (int i=0;i<2;++i)
    {
        mask |= 0x01 << dma_channel[i];
    }

    dma_set_irq1_channel_mask_enabled(mask, true);
    irq_set_enabled(DMA_IRQ_1, true);

    // Build the pwm start mask
    uint32_t pwm_mask = 0;
    
    pwmChannelAddStartList(&pwm_channel[0], &pwm_mask);
    pwmChannelAddStartList(&pwm_channel[1], &pwm_mask);
    
    // Build the DMA start mask
    uint32_t chan_mask = 0x01 << dma_channel[0];

    // Initialise the buttons
    debounceButtonCreate(&button[0], 20, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[1], 21, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[2], 22, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[3], 14, 40, buttonCallback, false, true);

    // Create the event queue
    enum Event event = empty;
    queue_init(&eventQueue, sizeof(event), 4);

    // Set up and create the sound double buffers in RAM
    colourNoiseCreate(&cn[0], 0.5);
    colourNoiseSeed(&cn[0], 0);
    colourNoiseCreate(&cn[1], 0.5);
    colourNoiseSeed(&cn[1], 2^15-1);
    populateBuffer fn = &createNoise;
    //circularBufferCreate(&sb, WAV_DATA, WAV_DATA_LENGTH, shift);
    //populateBuffer fn = &getSound;

    // Start with coloured noise
    current_RAM_Buffer = doubleBufferCreate(&double_buffers, ram_buffer[0], ram_buffer[1], RAM_BUFFER_LENGTH, fn);

    // Populate the DMA buffers
    populateDmaBuffer();
    populateDmaBuffer();

    // Start the first DMA channel in the chain and both PWMs
    dma_start_channel_mask(chan_mask);
    pwmChannelStartList(pwm_mask);

    /*
     * Main loop Generate noise, handle buttons for volume, parse wav blocks etc
     */
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

            case populate_dma:
                populateDmaBuffer();
            break;

            case populate_double:
                doubleBufferPopulateNext(&double_buffers);
            break;

            case change:
                colour += 1;
                if (colour == max)
                {
                    colour = white;
                }
                else if (colour == file_1)
                {
                    printf("colour = file_1");
                    // Need to swap to file_1, but only if mounted
                    if (!attemptFile("PinkPanther60.wav"));
                    {
                        colour = white;
                    }
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

// Called when a button is pressed
void buttonCallback(uint gpio_number, enum debounce_event event)
{
    enum Event e = empty;

    switch (gpio_number)
    {
        case 14:
            e = change;
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

// Disable DMAs and PWMs
void pauseMusic()
{
        
    for (int i=0; i<2; ++i)
    {
        pwmChannelStop(&pwm_channel[i]);
    }

    for (int i=0; i<2; ++i)
    {
        dma_channel_abort(dma_channel[i]);
    }
}

void stopMusic()
{
    pauseMusic();
    unmount();
}

// Write coloured noise to supplied buffer
// callback function called from circular buffer class
// len is number of 16 bit samples to copy
void createNoise(uint16_t* buffer, uint len)
{
    switch (colour)
    {
        case white:
        {
            for (int i=0;i<len;i+=2)
            {
                buffer[i] = (uint16_t)((colourNoiseWhite(&cn[0]) + 0.5) * wrap);
                buffer[i+1] = (uint16_t)((colourNoiseWhite(&cn[1]) + 0.5) * wrap);
            }
        }
        break;

        case pink:
        {
            for (int i=0;i<len;i+=2)
            {
                buffer[i] = (uint16_t)((colourNoisePink(&cn[0]) + 0.5) * wrap);
                buffer[i+1] = (uint16_t)((colourNoisePink(&cn[1]) + 0.5) * wrap);
            }
        }
        break;

        case brown:
        {
            for (int i=0;i<len;i+=2)
            {
                buffer[i] = (uint16_t)((colourNoiseBrown(&cn[0]) + 0.5) * wrap);
                buffer[i+1] = (uint16_t)((colourNoiseBrown(&cn[1]) + 0.5) * wrap);
            }
        }
        break;
    }
}

// Populate next sound buffer from circular buffer held in Flash
// Callback function called from circular buffer class
// len is number of 16 bit samples to copy
void getSound(uint16_t* buffer, uint len)
{
    circularBufferRead(&sb, buffer, len);
}

void getFileSound(uint16_t* buffer, uint len)
{
    waveFileRead(&wf, buffer, len);
}

static bool mount(void)
{
    if (!mounted)
    {
        pSD = sd_get_by_num(0);
        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        if (FR_OK != fr)
        {
            printf("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        }
        else
        {
            printf("Mount ok\n");
            mounted = true;
        }
    }
    return mounted;
}

static void unmount(void)
{
    mounted = false;   
    f_unmount(pSD->pcName);
}

static bool attemptFile(const char* filename)
{
    bool ret = false;

    printf("in attempt file\n");

    if (mounted)
    {
        if (!waveFileCreate(&wf, &fil, filename))
        {
            printf("Cannot open file: %s\n", filename);
        }
        else
        {
            // File opened
            pauseMusic();

            getRepeatShift(wf.sample_rate, &repeat_shift, &wrap, &mid_point, &fraction);
            pwmChannelReconfigure(&pwm_channel[0], fraction, wrap);
            pwmChannelReconfigure(&pwm_channel[1], fraction, wrap);

            // Reininitialise the double buffers
            current_RAM_Buffer = doubleBufferRestart(&double_buffers, &getFileSound);

            // Populate the DMA buffers
            populateDmaBuffer();
            populateDmaBuffer();

            // Start the first DMA channel in the chain and both PWMs
            uint32_t pwm_mask = 0;
    
            pwmChannelAddStartList(&pwm_channel[0], &pwm_mask);
            pwmChannelAddStartList(&pwm_channel[1], &pwm_mask);
    
            // Build the DMA start mask
            uint32_t chan_mask = 0x01 << dma_channel[0];

            sleep_ms(5000);
            printf("restarting...");

            dma_start_channel_mask(chan_mask);
            pwmChannelStartList(pwm_mask);
        }
    }
    return ret;
}