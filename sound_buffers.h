#pragma once
#include "pico/stdlib.h"

// Function to populate buffer
typedef void (*populateBuffer)(uint16_t* pBuffer, uint buffer_len, int id);

// Data for buffers
typedef struct sound_buffers
{
    uint      buffer_number;         // Buffer that was last filled
    uint16_t* buffers[2];            // Address of buffers
    uint      buffer_len;            // Length of buffers
    const uint16_t* source;          // Source to populate buffers
    uint      src_len;               // Length of source buffer
    uint      src_pos;               // Current read position in source buffer
    populateBuffer fn;               // Population function
    int       id;                    // Id passed to population function

} sound_buffers;

// Create the buffers
extern const uint16_t* soundBuffersCreateFlash(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, const uint16_t* source, uint src_len);
extern const uint16_t* soundBuffersCreateFunction(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, populateBuffer fn, int id);

// Populate the next buffer
extern void soundBuffersPopulateNext(sound_buffers* sb);

// Obtain the last populated buffer
inline const uint16_t* soundBufferGetLast(sound_buffers* sb){return sb->buffers[sb->buffer_number];}