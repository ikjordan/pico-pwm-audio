#include "debounce_button.h"

static debounce_button_data* debounce_data[32];
static void debounceButtonCallback(uint gpio, uint32_t events);
static int64_t debounceButtonTimerCallback(alarm_id_t id, void *user_data);

void debounceButtonCreate(debounce_button_data* db, uint pin, uint delay_ms, event_callback_t event_callback, bool up, bool high)
{
    debounce_data[pin] = db;
    db->pin = pin;
    db->delay_ms = delay_ms;
    db->event_callback = event_callback;
    db->up = up;
    db->high = high;
    db->timer_id = -1;

    gpio_init(db->pin);
    gpio_set_dir(db->pin, GPIO_IN);

    if (db->up)
    {
        gpio_pull_up(db->pin);
    }
    else
    {
        gpio_pull_down(db->pin);
    }

    // Attach the interrupt handler
    gpio_set_irq_enabled_with_callback(db->pin, db->high ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL, true, &debounceButtonCallback);
}

static void debounceButtonCallback(uint gpio, uint32_t events)
{
    // Get the data for this block
    debounce_button_data* db = debounce_data[gpio];
    if (db)
    {
            if (db->timer_id == -1)
            {
                // Timer not running so create one
                db->timer_id = add_alarm_in_ms(db->delay_ms, debounceButtonTimerCallback, db, true);
            }
    }
    gpio_acknowledge_irq(gpio, events);
}

// Called when the timer fires
// Send data back to the application
static int64_t debounceButtonTimerCallback(alarm_id_t id, void* db) 
{
    // Is the button pressed?
    if (gpio_get(((debounce_button_data*)db)->pin) == ((debounce_button_data*)db)->high)
    {
        ((debounce_button_data*)db)->event_callback(((debounce_button_data*)db)->pin, single_press);
    }

    // Clear timer indicator
    ((debounce_button_data*)db)->timer_id = -1;
    return 0;
}
