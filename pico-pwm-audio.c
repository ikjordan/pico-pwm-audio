#include <stdio.h>
#include <math.h>          // For fminf and fmaxf
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/dma.h"  // dma 
#include "hardware/sync.h" // wait for interrupt 
#include "pico/util/queue.h" 

#include "fs_mount.h"
#include "pwm_channel.h"
#include "debounce_button.h"
#include "double_buffer.h"
#include "circular_buffer.h"
#include "colour_noise.h"
#include "wave_file.h"

 
#define AUDIO_PIN 18  // Configured for the Maker board 18 left, 19 right
#define STEREO        // When stereo enabled, currently DMA same data to both channels
//#define FLASH

#ifdef FLASH
/* 
 * This include brings in static arrays which contain audio samples. 
 * if you want to know how to make these please see the python code
 * for converting audio samples into static arrays. 
 */
#include "ring.h"
#endif

#ifdef STEREO
bool stereo = true;
#else
bool stereo = false;
#endif

static colour_noise cn[2];
static circular_buffer sb;


#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11000
#endif
#define DMA_BUFFER_LENGTH 2200      // 2200 samples @ 44kHz gives= 0.05 seconds = interrupt rate

#define RAM_BUFFER_LENGTH (4*DMA_BUFFER_LENGTH)

/*
 * Static variable definitions
 */
#ifdef FLASH
#ifdef TWELVE_BIT
static const int flash_shift = 0;           // Only used for flash samples
#else
static const int flash_shift = 3;           // Only used for flash samples
#endif
#endif

static uint wrap;                           // Largest value a sample can be + 1
static int mid_point;                       // wrap divided by 2
static float fraction = 1;                  // Divider used for PWM
static int repeat_shift = 1;                // Defined by the sample rate

static pwm_data pwm_channel[2];             // Represents the PWM channels
static int dma_channel[2];                  // The 2 DMA channels used for DMA ping pong

 // Have 2 buffers in RAM that are used to DMA the samples to the PWM engine
static uint32_t dma_buffer[2][DMA_BUFFER_LENGTH];
static int dma_buffer_index = 0;            // Index into active DMA buffer

// Have 2 or 4 8k buffers in RAM, copy data from Flash to these buffers - in future
// will be buffers where noise is created, or music delivered from SD Card

// RAM buffers, controlled through double_buffer class
static uint16_t ram_buffer[2][RAM_BUFFER_LENGTH];
static int ram_buffer_index = 0;            // Holds current position in ram_buffers for channels

// Control data blocks for the RAM double buffers
static double_buffer double_buffers;
void populateCallback(uint16_t* buffer, uint len);   // Call back to generate next buffer of sound

// Pointer to the currenly in use RAM buffer
static const uint16_t* current_RAM_Buffer = 0;

static float volume = 0.8;                  // Initial volume adjust, controlled by button

// Event queue, used to leave ISR context
static queue_t eventQueue;

// Supported events
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

// Range of sound colours and files that can be played
enum sound_state
{
    off = 0,
    start = off + 1,
    brown = start,
    file_1 = brown + 1,
    file_2 = file_1 + 1,
    file_3 = file_2 + 1,
#ifdef FLASH    
    flash = file_3 + 1,
    white = flash + 1,
#else
    white = file_3 + 1,
#endif
    pink = white + 1,
    end = pink + 1
};

// Helper to determine if state is a colour state
static inline bool isColour(enum sound_state state) {return (state == white || state == pink || state == brown);}
static inline bool isFile(enum sound_state state) {return (state == file_1 || state == file_2 || state == file_3);}

static void changeState(enum sound_state new_state);
enum sound_state current_state = off; 

// Four buttons
static debounce_button_data button[4];

/* 
 * Function declarations
 */
static void populateDmaBuffer(void);
static void claimDmaChannels(int num_channels);
static void initDma(int buffer_index, int slice, int chain_index);
static void dmaInterruptHandler();

static bool getSampleValues(uint sample_rate, uint* shift, uint* wrap, uint* mid_point, float* fraction);

void startMusic(uint32_t sample_rate);
void stopMusic();
void exitMusic();

void buttonCallback(uint gpio_number, enum debounce_event event);

static bool loadFile(const char* filename);
static fs_mount mount;
static wave_file wf;

#define FILE_NAME_1 "1.wav"
#define FILE_NAME_2 "2.wav"
#define FILE_NAME_3 "3.wav"

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
#ifdef VOLUME        
        uint32_t left = ((current_RAM_Buffer[(wav_position>>repeat_shift)<<1]) - mid_point) * volume + mid_point;
        uint32_t right = ((current_RAM_Buffer[((wav_position>>repeat_shift)<<1)+1]) - mid_point) * volume + mid_point;
#else
        uint32_t left = current_RAM_Buffer[(ram_buffer_index>>repeat_shift)<<1];
        uint32_t right = current_RAM_Buffer[((ram_buffer_index>>repeat_shift)<<1)+1];
#endif        
        ram_buffer_index++;

        if (!stereo)
        {
            // Want mono, so average two channels
            left = (left + right) >> 1;
            right = left;
        }

        // Combine the two channels
        dma_buffer[dma_buffer_index][i] = (right << 16) + left;

        if ((ram_buffer_index<<1) == (RAM_BUFFER_LENGTH<<repeat_shift)) 
        {
            // Need a new RAM buffer
            current_RAM_Buffer = doubleBufferGetLast(&double_buffers);

            // reset read position of RAM buffer to start
            ram_buffer_index = 0;

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

// Determin configuration data, based on sample rate
static bool getSampleValues(uint sample_rate, uint* shift, uint* wrap, uint* mid_point, float* fraction)
{
    bool ret = true;

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

        case 12000:
            *shift = 2;
            *wrap = 3750;
            *fraction = 1.0f;
        break;

        case 24000:
            *shift = 1;
            *wrap = 3750;
            *fraction = 1.0f;
        break;

        case 48000:
            *shift = 0;
            *wrap = 3750;
            *fraction = 1.0f;
        break;

        default:
            // Not a supported rate
            *wrap = 0;
            ret = false;
        break;
    }

    // mid point is half of wrap value
    *mid_point = *wrap >> 1;

    return ret;
}

int main(void) 
{
    // Overclock to 180MHz so that system clock is a multiple of typical
    // audio sampling rates
    if (!set_sys_clock_khz(180000, true))
    {
        panic("Cannot set clock rate\n");
    }   
    
    // Adjust frequency before initialiing, so serial port will work
    stdio_init_all();

    // Set up the PWMs with arbiraty values, will be updates when play starts
    pwmChannelInit(&pwm_channel[0], AUDIO_PIN);
    pwmChannelInit(&pwm_channel[1], AUDIO_PIN+1);

    // Get the DMA channels for the chain
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

    // Initialise the buttons
    debounceButtonCreate(&button[0], 20, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[1], 21, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[2], 22, 40, buttonCallback, true, false);
    debounceButtonCreate(&button[3], 14, 40, buttonCallback, false, true);

    // Create the event queue
    enum Event event = empty;
    queue_init(&eventQueue, sizeof(event), 4);

    // Set up noise and flash buffer
    colourNoiseCreate(&cn[0], 0.5);
    colourNoiseSeed(&cn[0], 0);
    colourNoiseCreate(&cn[1], 0.5);
    colourNoiseSeed(&cn[1], 2^15-1);
#ifdef FLASH    
    circularBufferCreate(&sb, WAV_DATA, WAV_DATA_LENGTH, flash_shift);
#endif
    // Create the double buffers
    doubleBufferCreate(&double_buffers, ram_buffer[0], ram_buffer[1], RAM_BUFFER_LENGTH);

    // Initialise the file system
    fsInitialise(&mount);
    fsMount(&mount);

    // Start by playing brown noise
    changeState(brown);

    /*
     * Main loop Generate noise, handle buttons for volume, parse wav blocks etc
     */
    // Process events
    while (true)
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
                changeState(current_state + 1);
            break;

            case quit:
                exitMusic();
            break;

            default:
                return -1;
            break;
        }
    }
    return 0;
}

static void changeState(enum sound_state new_state)
{
    // Handle wrap
    if (new_state == end)
    {
        new_state = start;
    }

    // Stop playing if we are, and close the file if it is open
    if (current_state != off)
    {
        stopMusic();

        // Close the file, if it was open
        if (isFile(current_state))
        {
            waveFileClose(&wf);
        }
    }

    // If moving to file state try to open the file
    if (new_state == file_1)
    {
        if (!loadFile(FILE_NAME_1))
        {
            new_state += 1;
        }
    }

    if (new_state == file_2)
    {
        if (!loadFile(FILE_NAME_2))
        {
            new_state += 1;
        }
    }

    if (new_state == file_3)
    {
        if (!loadFile(FILE_NAME_3))
        {
            new_state += 1;
        }
    }

    // Handle the case where noise is not supported
    if (new_state == end)
    {
        new_state = start;
    }

    // State needs to be changed before buffers populated
    current_state = new_state;

    // Now in a position to start playing the sound
    uint32_t sample_rate;

    if (isColour(current_state))
    {
        sample_rate = SAMPLE_RATE;
    }
    else if (isFile(current_state))
    {
        printf("Sample rate is %u\n", wf.sample_rate);
        sample_rate = wf.sample_rate;
    }
    else // Loaded from flash
    {
        sample_rate = SAMPLE_RATE;
    }
    startMusic(sample_rate);
}

void startMusic(uint32_t sample_rate)
{
    // Empty the message queue, to avoid processing populate messages
    enum Event skip = empty;
    while(queue_try_remove(&eventQueue, &skip));

    // Reconfigure the PWM for the new wrap and clock
    getSampleValues(sample_rate, &repeat_shift, &wrap, &mid_point, &fraction);
    pwmChannelReconfigure(&pwm_channel[0], fraction, wrap);
    pwmChannelReconfigure(&pwm_channel[1], fraction, wrap);

    // Reininitialise the double buffers
    current_RAM_Buffer = doubleBufferInitialise(&double_buffers, &populateCallback);

    // reset read position of RAM buffer to start
    ram_buffer_index = 0;

    // Populate the DMA buffers
    populateDmaBuffer();
    populateDmaBuffer();

    // Start the first DMA channel in the chain and both PWMs
    uint32_t pwm_mask = 0;

    pwmChannelAddStartList(&pwm_channel[0], &pwm_mask);
    pwmChannelAddStartList(&pwm_channel[1], &pwm_mask);

    // Build the DMA start mask
    uint32_t chan_mask = 0x01 << dma_channel[0];

    dma_start_channel_mask(chan_mask);
    pwmChannelStartList(pwm_mask);
}

void stopMusic(void)
{
    // Disable DMAs and PWMs
    pwmChannelStop(&pwm_channel[0]);
    pwmChannelStop(&pwm_channel[1]);

    dma_channel_abort(dma_channel[0]);
    dma_channel_abort(dma_channel[1]);
}

void exitMusic(void)
{
    // Stop music and unmount the file system
    stopMusic();
    fsUnmount(&mount);
    current_state = off;
}


// Write 16 bit stereo sound data to to the supplied buffer
// callback function called from circular buffer class
// len is number of 16 bit samples to copy
void populateCallback(uint16_t* buffer, uint len)
{
    switch (current_state)
    {
        case white:
            for (int i=0;i<len;i+=2)
            {
                // Divide the output by 2, to make similar volume to other colours
                buffer[i] = (uint16_t)((colourNoiseWhite(&cn[0]) + 0.5) * (wrap >> 1));
                buffer[i+1] = (uint16_t)((colourNoiseWhite(&cn[1]) + 0.5) * (wrap >> 1));
            }
        break;

        case pink:
            for (int i=0;i<len;i+=2)
            {
                buffer[i] = (uint16_t)((colourNoisePink(&cn[0]) + 0.5) * wrap);
                buffer[i+1] = (uint16_t)((colourNoisePink(&cn[1]) + 0.5) * wrap);
            }
        break;

        case brown:
            for (int i=0;i<len;i+=2)
            {
                buffer[i] = (uint16_t)((colourNoiseBrown(&cn[0]) + 0.5) * wrap);
                buffer[i+1] = (uint16_t)((colourNoiseBrown(&cn[1]) + 0.5) * wrap);
            }
        break;

#ifdef FLASH
        case flash:
            circularBufferRead(&sb, buffer, len);
        break;
#endif    
        default:
            if (isFile(current_state))
            {
                waveFileRead(&wf, buffer, len);
            }
        break;
    }
}

static bool loadFile(const char* filename)
{
    bool success = false;

    if (fsMount(&mount))
    {
        if (!waveFileCreate(&wf, filename))
        {
            printf("Cannot open file: %s\n", filename);
        }   
        else
        {
            success = true;
        }
    }
    return success;
}

// Called when a button is pressed
void buttonCallback(uint gpio_number, enum debounce_event event)
{
    enum Event e = empty;

    switch (gpio_number)
    {
        case 20:
            e = change;
        break;

        case 14:
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

