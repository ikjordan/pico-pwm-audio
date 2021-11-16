#pragma once
#include "pico/stdlib.h"

// Data for circular buffer
typedef struct circular_buffer
{
    const int16_t* buffer;           // Address of buffer
    uint      buffer_len;            // Length of buffer
    uint      shift;                 // Shift to adjust range of data
    uint      pos;                   // Current read position in buffer
} circular_buffer;

// Create the buffers
extern void circularBufferCreate(circular_buffer* cb, const int16_t* buff, uint buffer_len, uint shift);

// Populate destination from the circular buffer
extern void circularBufferRead(circular_buffer* cb, int16_t* dest, uint len);

