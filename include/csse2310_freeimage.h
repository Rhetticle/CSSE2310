// csse2310_freeimage.h
//
#ifndef CSSE2310_FREEIMAGE_H
#define CSSE2310_FREEIMAGE_H

#include <FreeImage.h>
FIBITMAP* fi_load_image_from_buffer(const unsigned char* buffer, unsigned long numBytes);
unsigned char* fi_save_png_image_to_buffer(FIBITMAP* bitmap, unsigned long* numBytes);

#endif
