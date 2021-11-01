/**
 * Read and parse a wave file
 *
 **/
#include <stdio.h>
#include <string.h>
#include "f_util.h"
#include "ff.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include "wave.h"

#define READ_BUFFER 4096
#define DATA_BUFFER 2000

unsigned char read_buffer[READ_BUFFER];
int16_t* read_buffer_16 = (int16_t*)read_buffer;
int32_t* read_buffer_32 = (int32_t*)read_buffer;

uint16_t data_buffer[2][DATA_BUFFER];

// Function declarations
static void file_read(FIL* fil, void* buffer, uint size, const char* msg);

int parse_wav(const char* filename)
{
    // WAVE header structure
    struct HEADER header;

    unsigned char buffer[4];
    uint32_t val32;
    uint16_t val16;
    FIL fil;
    FRESULT fr;

    // open file
    printf("\nOpening  file: %s\n", filename);
    fr = f_open(&fil, filename, FA_OPEN_EXISTING | FA_READ);

    if (FR_OK != fr)
    {
        printf("Error opening file\n");
        return -1;
    }
 
    int read = 0;
    
    // http://soundfile.sapp.org/doc/WaveFormat/

    // ChunkID
    file_read(&fil, buffer, 4, "ChunkID");
    printf("(0-3)   Chunk ID: %c%c%c%c\n", buffer[0], buffer[1], buffer[2], buffer[3]);
    
    if (buffer[0] != 'R' || buffer[1] != 'I' || buffer[2] != 'F' || buffer[3] != 'F')
    {
        printf("Not RIFF file\n");
        return -1;
    }

    // ChunkSize
    file_read(&fil, &val32, sizeof(val32), "ChunkSize");
    printf("(4-7)   ChunkSize: bytes: %u, Kb: %u\n", val32, val32/1024);

    // Format
   file_read(&fil, buffer, 4, "Format");    
   printf("(8-11)  Format: %c%c%c%c\n", buffer[0], buffer[1], buffer[2], buffer[3]);
    
    if (buffer[0] != 'W' || buffer[1] != 'A' || buffer[2] != 'V' || buffer[3] != 'E')
    {
        printf("Not WAV file\n");
        return -1;
    }

    // Subchunk1ID
    file_read(&fil, buffer, 4, "Subchunk1ID");    
    printf("(12-15) Fmt marker: %c%c%c%c\n", buffer[0], buffer[1], buffer[2], buffer[3]);
    
    if (buffer[0] != 'f' || buffer[1] != 'm' || buffer[2] != 't' || buffer[3] != ' ')
    {
        printf("Unknown format\n");
        return -1;
    }

    // Subchunk1Size
    file_read(&fil, &val32, sizeof(val32), "Subchunk1Size");    
    printf("(16-19) Subchunk1Size: %u\n", val32);
    
    if (val32 != 16)
    {
        printf("Unexpected Subchunk1Size\n");
        return -1;
    }

    // AudioFormat
    file_read(&fil, &val16, sizeof(val16),"AudioFormat");    
    char format_name[10] = "";
    
    if (val16 == 1)
        strcpy(format_name,"PCM"); 
    else if (val16 == 6)
        strcpy(format_name, "A-law");
    else if (val16 == 7)
        strcpy(format_name, "Mu-law");

    printf("(20-21) Format type: %u %s\n", val16, format_name);
    
    if (val16 != 1)
    {
        printf("Only PCM supported\n");
        return -1;
    }

    // NumChannels
    file_read(&fil, &header.channels, sizeof(header.channels), "NumChannels");    
    printf("(22-23) Channels: %u\n", header.channels);
    
    if (header.channels > 2 || header.channels < 1)
    {
        printf("Unsupported number of channels\n");
        return -1;
    }

    // SampleRate
    file_read(&fil, &header.sample_rate, sizeof(header.sample_rate), "SampleRate");    
    printf("(24-27) Sample rate: %u\n", header.sample_rate);
    
    if (header.sample_rate < 8000 || header.sample_rate > 44100)
    {
        printf("Unsupported sample rate\n");
    }

    // Byte rate
    file_read(&fil, &val32, sizeof(val32), "Byte rate");    
    printf("(28-31) Byte Rate: %u\n", val32);

    // Block align
    file_read(&fil, &val16, sizeof(val16), "Block align");    
    printf("(32-33) Block Alignment: %u\n", val16);

    // Bits per sample
    file_read(&fil, &header.bits_per_sample, sizeof(header.bits_per_sample), "Bits per sample");    
    printf("(34-35) Bits per sample: %u\n", header.bits_per_sample);

    // Subchunk2ID
    file_read(&fil, buffer, 4, "Subchunk2ID");    
    printf("(36-39) Data marker: %c%c%c%c\n", buffer[0], buffer[1], buffer[2], buffer[3]);
    
    if (buffer[0] != 'd' || buffer[1] != 'a' || buffer[2] != 't' || buffer[3] != 'a')
    {
        printf("Unknown format - aborting\n");
        return -1;
    }

    // Subchunk2Size
    file_read(&fil, &header.data_size, sizeof(header.data_size), "Subchunk2Size");    
    printf("(40-43) Subchunk2Size: %u\n", header.data_size);
    
    // calculate no.of samples
    uint num_samples = (header.data_size) / (header.channels * header.bits_per_sample / 8);
    printf("Number of samples: %u\n", num_samples);

    int size_of_each_sample = (header.channels * header.bits_per_sample) / 8;
    printf("Size of each sample: %d bytes\n", size_of_each_sample);

    // calculate duration of file
    float duration_in_seconds = (float) header.data_size / (header.channels * header.sample_rate * header.bits_per_sample / 8);
    printf("Duration in seconds = %f\n", duration_in_seconds);

    // Want to read in chunks - filling the read_buffer
    uint32_t samples_left = num_samples;
    uint32_t samples_to_read;
    uint32_t max_samples_to_read = READ_BUFFER / size_of_each_sample;
    uint32_t total_read = 0;
    uint32_t data_index = 0;

    // How many samples will fill in the read buffer?
    while (samples_left)
    {
        samples_to_read = (max_samples_to_read < samples_left) ? max_samples_to_read : samples_left;

        FRESULT fr = f_read(&fil, read_buffer, samples_to_read * size_of_each_sample, &read);
        if (FR_OK != fr || read != (size_of_each_sample * samples_to_read))
        {
            printf("Read: %i Expected: %i\n", read, size_of_each_sample * samples_to_read);
            panic("Error in f_read of sample %i \n", read);
        }
        // Write the samples - store as 16 bit unsigned values - will also bound here
        switch (header.bits_per_sample)
        {
            case 8:
                for (int i = 0; i < samples_to_read; ++i)
                {
                    data_buffer[0][data_index] = ((uint16_t)(read_buffer[i])) << 4;
                    data_buffer[1][data_index] = (header.channels == 2) ? ((uint16_t)(read_buffer[i])) << 4: data_buffer[0][data_index];
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            break;

            case 16:
                for (int i = 0; i < samples_to_read; ++i)
                {
                    data_buffer[0][data_index] = (read_buffer_16[i] + 0x8000) >> 4;
                    data_buffer[1][data_index] = (header.channels == 2) ? ((read_buffer_16[i] + 0x8000) >> 4): data_buffer[0][data_index];
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            break;

            case 32:
            {
                uint32_t temp_l;
                uint32_t temp_r;
                for (int i = 0; i < samples_to_read; ++i)
                {
                    temp_l = (read_buffer_32[i] + 0x80000000) >> 20;
                    temp_r = (header.channels == 2) ? ((read_buffer_32[i] + 0x80000000) >> 20): temp_l;
                    data_buffer[0][data_index] = (uint16_t)temp_l;
                    data_buffer[1][data_index] = (uint16_t)temp_r;
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            }
            break;
        }

        samples_left -= samples_to_read;
    }

    printf("Closing file..\n");
    fr = f_close(&fil);

    if (FR_OK != fr) 
    {
        panic("f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }

    return 0;
}

static void file_read(FIL* fil, void* buffer, uint size, const char* msg)
{
    uint read;

    FRESULT fr = f_read(fil, buffer, size, &read);
    if (FR_OK != fr || read != size)
    {
        panic("Error in f_read %s \n", msg);
    }
}