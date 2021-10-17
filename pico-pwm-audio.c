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
#include "sample.h"
//#include "thats_cool.h"
int wav_position = 0;

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
    if (wav_position < (WAV_DATA_LENGTH<<3) - 1) 
    { 
        wav_position++;
    } else 
    {
        // reset to start
        wav_position = 0;
    }
    pwm_set_gpio_level(AUDIO_PIN, WAV_DATA[wav_position>>3]); 
#ifdef STEREO    
    pwm_set_gpio_level(AUDIO_PIN + 1, WAV_DATA[wav_position>>3]); 
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
    /* Base clock 176,000,000 Hz divide by wrap 250 then the clock divider further divides
     * to set the interrupt rate. 
     * 
     * 11 KHz is fine for speech. Phone lines generally sample at 8 KHz
     * 
     * 
     * So clkdiv should be as follows for given sample rate
     *  8.0f for 11 KHz
     *  4.0f for 22 KHz
     *  2.0f for 44 KHz etc
     */
    // Clock rate is 176 MHz
    // In theory have a range of 2^16, but in reality it is limited to wrap
    // If we limit wrap to 250, then the duty cycle has to be in range 0 to 249 - larger values will be clipped

    // Reduces the effective clock to 176 / 8 = 22 MHz
    pwm_config_set_clkdiv(&config, 8.0f); 

    // 250 samples at 22 MHz is 88 KHz
    // For 11 kHz we then repeat the sample 8 times in the isr - to give 11 kHz
    pwm_config_set_wrap(&config, 250); 

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
