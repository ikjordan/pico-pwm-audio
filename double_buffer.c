#include "double_buffer.h"
/*
   Manages double buffers in RAM.
   These are filled by calling a populate function
 */

// Create the buffers and populate using supplied function
void doubleBufferCreate(double_buffer* db, uint16_t* buff0, uint16_t* buff1, uint buffer_len)
{
     // The buffers are passed this way to allow for case where they are not contiguous
    db->buffers[0] = buff0;
    db->buffers[1] = buff1;

    db->buffer_len = buffer_len;
    db->fn = NULL;
}

const uint16_t* doubleBufferInitialise(double_buffer* db, populateBuffer fn)
{
    db->fn = fn;
    db->buffer_number = 1;  // Starts as 1, as swapped before first populate

    // Fill both of the buffers
    doubleBufferPopulateNext(db);
    doubleBufferPopulateNext(db);

    // return a pointer to the first buffer
    return db->buffers[0];
}

// Populate the next buffer
void doubleBufferPopulateNext(double_buffer* db)
{
    // Swap the active buffer number
    db->buffer_number = 1 - db->buffer_number;

    // Use the callback to populate the buffer
    if (db->fn)
    {
        (*(db->fn))(db->buffers[db->buffer_number], db->buffer_len);
    }
}
