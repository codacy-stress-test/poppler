//========================================================================
//
// PNGWriter.cc
//
// This file is licensed under the GPLv2 or later
//
// Copyright (C) 2009 Warren Toomey <wkt@tuhs.org>
// Copyright (C) 2009 Shen Liang <shenzhuxi@gmail.com>
// Copyright (C) 2009, 2011-2023 Albert Astals Cid <aacid@kde.org>
// Copyright (C) 2009 Stefan Thomas <thomas@eload24.com>
// Copyright (C) 2010, 2011, 2013, 2017 Adrian Johnson <ajohnson@redneon.com>
// Copyright (C) 2011 Thomas Klausner <wiz@danbala.tuwien.ac.at>
// Copyright (C) 2012 Pino Toscano <pino@kde.org>
//
//========================================================================

#include "PNGWriter.h"

#ifdef ENABLE_LIBPNG

#    include <zlib.h>
#    include <cstdlib>
#    include <cstring>

#    include "poppler/Error.h"
#    include "goo/gmem.h"

#    include <png.h>

struct PNGWriterPrivate
{
    explicit PNGWriterPrivate(PNGWriter::Format f) : format(f) { }

    PNGWriter::Format format;
    png_structp png_ptr = nullptr;
    png_infop info_ptr = nullptr;
    unsigned char *icc_data = nullptr;
    int icc_data_size = 0;
    char *icc_name = nullptr;
    bool sRGB_profile = false;

    PNGWriterPrivate(const PNGWriterPrivate &) = delete;
    PNGWriterPrivate &operator=(const PNGWriterPrivate &) = delete;
};

PNGWriter::PNGWriter(Format formatA)
{
    priv = new PNGWriterPrivate(formatA);
}

PNGWriter::~PNGWriter()
{
    /* cleanup heap allocation */
    png_destroy_write_struct(&priv->png_ptr, &priv->info_ptr);
    if (priv->icc_data) {
        gfree(priv->icc_data);
        free(priv->icc_name);
    }

    delete priv;
}

void PNGWriter::setICCProfile(const char *name, unsigned char *data, int size)
{
    priv->icc_data = (unsigned char *)gmalloc(size);
    memcpy(priv->icc_data, data, size);
    priv->icc_data_size = size;
    priv->icc_name = strdup(name);
}

void PNGWriter::setSRGBProfile()
{
    priv->sRGB_profile = true;
}

bool PNGWriter::init(FILE *f, int width, int height, double hDPI, double vDPI)
{
    /* libpng changed the png_set_iCCP() prototype in 1.5.0 */
#    if PNG_LIBPNG_VER < 10500
    png_charp icc_data_ptr = (png_charp)priv->icc_data;
#    else
    png_const_bytep icc_data_ptr = (png_const_bytep)priv->icc_data;
#    endif

    if (hDPI < 0 || vDPI < 0) {
        error(errInternal, -1, "PNGWriter::init: hDPI or vDPI values are invalid {0:f} {1:f}", hDPI, vDPI);
        return false;
    }

    const double png_res_x = hDPI / 0.0254;
    const double png_res_y = vDPI / 0.0254;
    if (png_res_x > std::numeric_limits<png_uint_32>::max() || png_res_y > std::numeric_limits<png_uint_32>::max()) {
        error(errInternal, -1, "PNGWriter::init: hDPI or vDPI values are invalid {0:f} {1:f}", hDPI, vDPI);
        return false;
    }

    /* initialize stuff */
    priv->png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!priv->png_ptr) {
        error(errInternal, -1, "png_create_write_struct failed");
        return false;
    }

    priv->info_ptr = png_create_info_struct(priv->png_ptr);
    if (!priv->info_ptr) {
        error(errInternal, -1, "png_create_info_struct failed");
        return false;
    }

    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "png_jmpbuf failed");
        return false;
    }

    /* write header */
    png_init_io(priv->png_ptr, f);
    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "Error during writing header");
        return false;
    }

    // Set up the type of PNG image and the compression level
    png_set_compression_level(priv->png_ptr, Z_BEST_COMPRESSION);

    // Silence silly gcc
    png_byte bit_depth = -1;
    png_byte color_type = -1;
    switch (priv->format) {
    case RGB:
        bit_depth = 8;
        color_type = PNG_COLOR_TYPE_RGB;
        break;
    case RGB48:
        bit_depth = 16;
        color_type = PNG_COLOR_TYPE_RGB;
        break;
    case RGBA:
        bit_depth = 8;
        color_type = PNG_COLOR_TYPE_RGB_ALPHA;
        break;
    case GRAY:
        bit_depth = 8;
        color_type = PNG_COLOR_TYPE_GRAY;
        break;
    case MONOCHROME:
        bit_depth = 1;
        color_type = PNG_COLOR_TYPE_GRAY;
        break;
    }
    png_byte interlace_type = PNG_INTERLACE_NONE;

    png_set_IHDR(priv->png_ptr, priv->info_ptr, width, height, bit_depth, color_type, interlace_type, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_set_pHYs(priv->png_ptr, priv->info_ptr, static_cast<png_uint_32>(png_res_x), static_cast<png_uint_32>(png_res_y), PNG_RESOLUTION_METER);

    if (priv->icc_data) {
        png_set_iCCP(priv->png_ptr, priv->info_ptr, priv->icc_name, PNG_COMPRESSION_TYPE_BASE, icc_data_ptr, priv->icc_data_size);
    } else if (priv->sRGB_profile) {
        png_set_sRGB(priv->png_ptr, priv->info_ptr, PNG_sRGB_INTENT_RELATIVE);
    }

    png_write_info(priv->png_ptr, priv->info_ptr);
    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "error during writing png info bytes");
        return false;
    }

    return true;
}

bool PNGWriter::writePointers(unsigned char **rowPointers, int rowCount)
{
    png_write_image(priv->png_ptr, rowPointers);
    /* write bytes */
    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "Error during writing bytes");
        return false;
    }

    return true;
}

bool PNGWriter::writeRow(unsigned char **row)
{
    // Write the row to the file
    png_write_rows(priv->png_ptr, row, 1);
    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "error during png row write");
        return false;
    }

    return true;
}

bool PNGWriter::close()
{
    /* end write */
    png_write_end(priv->png_ptr, priv->info_ptr);
    if (setjmp(png_jmpbuf(priv->png_ptr))) {
        error(errInternal, -1, "Error during end of write");
        return false;
    }

    return true;
}

#endif
