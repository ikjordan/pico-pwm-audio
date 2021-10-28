#include "double_buffer.h"
/*
   Manages double buffers in RAM.
   These are filled by calling a populate function
 */

//local functions
static const uint16_t* initialiseBuffers(double_buffer* db);

// Create the buffers and populate using supplied function
const uint16_t* doubleBufferCreate(double_buffer* db, uint16_t* buff0, uint16_t* buff1, uint buffer_len, populateBuffer fn, int id)
{
    db->buffer_number = 1;  // Starts as 1, as swapped before first populate

    // The buffers are passed this way to allow for case where they are not contiguous
    db->buffers[0] = buff0;
    db->buffers[1] = buff1;

    db->buffer_len = buffer_len;
    db->fn = fn;
    db->id = id;

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
        (*(db->fn))(db->buffers[db->buffer_number], db->buffer_len, db->id);
    }
}
