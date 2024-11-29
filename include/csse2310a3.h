/*
 * csse2310a3.h
 */

#ifndef CSSE2310A3_H
#define CSSE2310A3_H

#include <stdio.h>
#include <stdint.h>

// Structure to hold a .uqz archive file header section
typedef struct {
    uint8_t  method;
    uint32_t numFiles;
    uint32_t* fileRecordOffsets; // one for each file (0 to numFiles - 1)
} UqzHeaderSection;

// read_uqz_header_section()
//      Reads the header section for a .uqz archive file from the given stream.
//      The stream must be open for reading and positioned at the beginning of
//      the file. Memory is allocated for the structure that is returned (and
//      the array of offsets within that strucutre). Returns NULL (and no memory
//      is allocated) if EOF is found before a complete header is read. The 
//      validity of the values returned is not checked. 
UqzHeaderSection* read_uqz_header_section(FILE* stream);

// free_uqz_header_section()
//      Frees the memory associated with a header section structure
void free_uqz_header_section(UqzHeaderSection* header);

#endif
