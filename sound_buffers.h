#pragma once
#include "pico/stdlib.h"

typedef struct sound_buffers
{
    uint      buffer_number;         // Buffer that was last filled
    uint16_t* buffers[2];            // Address of buffers
    uint      buffer_len;            // Length of buffers
    const uint16_t* source;          // Source to populate buffers
    uint      src_len;               // Length of source buffer
    uint      src_pos;               // Current read position in source buffer
} sound_buffers;

// Create the buffers
extern const uint16_t* soundBuffersCreate(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, const uint16_t* source, uint src_len);

// Populate the next buffer
extern void soundBuffersPopulateNext(sound_buffers* sb);

// Obtain the last populated buffer
inline const uint16_t* soundBufferGetLast(sound_buffers* sb){return sb->buffers[sb->buffer_number];}