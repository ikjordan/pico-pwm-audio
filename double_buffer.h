#pragma once
#include "pico/stdlib.h"

// Function to populate buffer
typedef void (*populateBuffer)(uint16_t* pBuffer, uint buffer_len);

// Data for buffers
typedef struct double_buffer
{
    uint      buffer_number;         // Buffer that was last filled
    uint16_t* buffers[2];            // Address of buffers
    uint      buffer_len;            // Length of buffers
    populateBuffer fn;               // Population function
} double_buffer;

// Create the buffers
extern const uint16_t* doubleBufferCreate(double_buffer* db, uint16_t* buff0, uint16_t* buff1, uint buffer_len, populateBuffer fn);

// Restart the buffers
extern const uint16_t* doubleBufferRestart(double_buffer* db, populateBuffer fn);

// Populate the next buffer
extern void doubleBufferPopulateNext(double_buffer* db);

// Obtain the last populated buffer
inline const uint16_t* doubleBufferGetLast(double_buffer* db){return db->buffers[db->buffer_number];}