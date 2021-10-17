#include <stdio.h>
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/pwm.h"  // pwm 
#include "hardware/sync.h" // wait for interrupt 
 
// Audio PIN is to match some of the design guide shields. 
#define AUDIO_PIN 18  // you can change this to whatever you like

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
int wav_position = 0;

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11000
#endif

#ifdef TWELVE_BIT
int shift = 0;
#else
int shift = 3;
#endif
int repeat_shift = 1;

/*
 * PWM Interrupt Handler which outputs PWM level and advances the 
 * current sample. 
 * 
 * We repeat the same value for 8 cycles this means sample rate etc
 * adjust by factor of 8 (this is what bitshifting <<3 is doing)
 * 
 */
void pwm_interrupt_handler() {
    // We have serviced the interrupt, so clear it
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));    

    // Update the position within the buffer
    if (wav_position < (WAV_DATA_LENGTH<<repeat_shift) - 1) 
    { 
        wav_position++;
    } else 
    {
        // reset to start
        wav_position = 0;
    }
    pwm_set_gpio_level(AUDIO_PIN, WAV_DATA[wav_position>>repeat_shift] << shift); 
#ifdef STEREO    
    pwm_set_gpio_level(AUDIO_PIN + 1, WAV_DATA[wav_position>>repeat_shift] << shift); 
#endif
}

int main(void) {
    /* Overclocking for fun but then also so the system clock is a 
     * multiple of typical audio sampling rates.
     */
    stdio_init_all();

    // Overclock to 176MHz - assert if cannot be set
    set_sys_clock_khz(176000, true);   

    // Set the audio pin to PWM
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM); 
#ifdef STEREO    
    gpio_set_function(AUDIO_PIN + 1, GPIO_FUNC_PWM); 
#endif
    // Get which of the 8 pwm slices has been assigned to the PWM
    int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);
#ifdef STEREO    
    int higher_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN + 1);
#endif
    // Setup PWM interrupt to fire when PWM cycle is complete
    // Remove any interrupt that exists - note that the interrupt is associated with the slice number
    pwm_clear_irq(audio_pin_slice);

    // Enable the interrupt for the PWM slice
    pwm_set_irq_enabled(audio_pin_slice, true);
    
    // Set an exclusive handler for PWM wraps. i.e. this is the only handler allowed for this interrupt type.
    // If an interrupt handler is already associated then this will assert
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler); 

    // Enable this interrupt
    irq_set_enabled(PWM_IRQ_WRAP, true);

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
    pwm_config_set_wrap(&config, 4000); 

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

    // Set the initial value of the pwm before start
    pwm_set_chan_level(pwm_gpio_to_slice_num(AUDIO_PIN), pwm_gpio_to_channel(AUDIO_PIN), WAV_DATA[0]);
#ifdef STEREO    
    pwm_set_chan_level(pwm_gpio_to_slice_num(AUDIO_PIN + 1), pwm_gpio_to_channel(AUDIO_PIN + 1), WAV_DATA[0]);
#endif
    // Start running
    pwm_init(audio_pin_slice, &config, true);
#ifdef STEREO    
    pwm_init(higher_pin_slice, &config, true);
#endif

    while(1) 
    {
        __wfi(); // Wait for Interrupt
    }
}
