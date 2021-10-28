#include "circular_buffer.h"
/*
   Manages a circular buffer
   This buffer can either be in RAM or Flash
 */

// Create the buffers
void circularBufferCreate(circular_buffer* cb, const uint16_t* buff, uint buffer_len)
{
    cb->buffer = buff;
    cb->buffer_len = buffer_len;
    cb->pos = 0;
}

// Populate destination from the circular buffer
void circularBufferRead(circular_buffer* cb, uint16_t* dest, uint len)
{
    for (int i=0; i<len ; ++i)
    {
        dest[i] = cb->buffer[cb->pos++];
        if (cb->pos == cb->buffer_len)
        {
            cb->pos = 0;
        }
    }
}