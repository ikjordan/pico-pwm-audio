#include "sound_buffers.h"
/*
   Maintains two sound buffers in RAM.
   These buffers can either be populated from falsh, or can be filled by calling a poulation function
 */

//local functions
static const uint16_t* initialiseBuffers(sound_buffers* sb);

// Create the buffers and populate from flash
const uint16_t* soundBuffersCreateFlash(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, const uint16_t* source, uint src_len)
{
    sb->buffer_number = 1;

    // The buffers are passed this way to allow for case where they are not contiguous
    sb->buffers[0] = buff0;
    sb->buffers[1] = buff1;

    sb->buffer_len = buffer_len;
    sb->source = source;
    sb->src_len = src_len;
    sb->src_pos = 0;
    sb->fn = NULL;

    return initialiseBuffers(sb);
}

// Create the buffers and populate using supplied function
const uint16_t* soundBuffersCreateFunction(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, populateBuffer fn, int id)
{
    sb->buffer_number = 1;

    // The buffers are passed this way to allow for case where they are not contiguous
    sb->buffers[0] = buff0;
    sb->buffers[1] = buff1;

    sb->buffer_len = buffer_len;
    sb->fn = fn;
    sb->id = id;

    return initialiseBuffers(sb);
}

static const uint16_t* initialiseBuffers(sound_buffers* sb)
{
    // populate the first buffer
    soundBuffersPopulateNext(sb);

    // Populate the second buffer
    soundBuffersPopulateNext(sb);

    // return a pointer to the first buffer
    return sb->buffers[0];
}


// Populate the next buffer
void soundBuffersPopulateNext(sound_buffers* sb)
{
    sb->buffer_number = 1 - sb->buffer_number;

    if (sb->fn)
    {
        (*(sb->fn))(sb->buffers[sb->buffer_number], sb->buffer_len, sb->id);
    }
    else
    {
        for (int i=0; i<sb->buffer_len;++i)
        {
            sb->buffers[sb->buffer_number][i] = sb->source[sb->src_pos++];

            if (sb->src_pos == sb->src_len)
            {
                sb->src_pos = 0;
            }
        }
    }
}
