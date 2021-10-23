#include "sound_buffers.h"

// Create the buffers
const uint16_t* soundBuffersCreate(sound_buffers* sb, uint16_t* buff0, uint16_t* buff1, uint buffer_len, const uint16_t* source, uint src_len)
{
    sb->buffer_number = 1;

    // The buffers are passed this way to allow for case where they are not contiguous
    sb->buffers[0] = buff0;
    sb->buffers[1] = buff1;

    sb->buffer_len = buffer_len;
    sb->source = source;
    sb->src_len = src_len;
    sb->src_pos = 0;

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

    for (int i=0; i<sb->buffer_len;++i)
    {
        sb->buffers[sb->buffer_number][i] = sb->source[sb->src_pos++];

        if (sb->src_pos == sb->src_len)
        {
            sb->src_pos = 0;
        }
    }
}
