#include "circular_buffer.h"
/*
   Manages a circular buffer
   This buffer can either be in RAM or Flash
 */

// Create the buffers
void circularBufferCreate(circular_buffer* cb, const int16_t* buff, uint buffer_len, uint shift)
{
    cb->buffer = buff;
    cb->buffer_len = buffer_len;
    cb->shift = shift;
    cb->pos = 0;
}

// Populate destination from the circular buffer
// len is the number of samples to copy
void circularBufferRead(circular_buffer* cb, int16_t* dest, uint len)
{
    for (int i=0; i<len ; ++i)
    {
        // Shift to full 16 bit unsigned, then convert to signed
        dest[i] = (cb->buffer[cb->pos++] << cb->shift) - 0x8000;
        if (cb->pos == cb->buffer_len)
        {
            cb->pos = 0;
        }
    }
}