/*
** asetexture.cpp
**
** Texture class for Aseprite images
**
**---------------------------------------------------------------------------
**
** Copyright 2026 Amy Robinson
** Copyright 2004-2016 Marisa Heit
** Copyright 2005-2019 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "basics.h"
#include "files.h"

#include "bitmap.h"
#include "filesystem.h"
#include "fs_files.h"
#include "image.h"
#include "imagehelpers.h"
#include "m_png.h"
#include "m_swap.h"
#include "printf.h"
#include "texturemanager.h"

//==========================================================================
//
// A PNG texture
//
//==========================================================================

class FASETexture : public FImageSource
{
  public:
	FASETexture(FileReader &lump, int lumpnum, int width, int height, int nFrames, uint8_t **frameBuffers);
	~FASETexture();

	int            CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override;
	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override;

  protected:
	void ReadAlphaRemap(FileReader *lump, uint8_t *alpharemap);
	void SetupPalette(FileReader &lump);

	uint8_t **framesData;
};

//==========================================================================
//
//
//
//==========================================================================

FImageSource *ASEImage_TryCreate(FileReader &data, int lumpnum)
{

	data.Seek(4, FileReader::SeekSet);
	uint16_t res = data.ReadUInt16();
	if (res != 0xa5e0)
		return NULL;

	int nFrames = data.ReadUInt16();

	int imWidth  = data.ReadUInt16();
	int imHeight = data.ReadUInt16();

	// Decompress the first cel - grab 1st frame
	data.Seek(128, FileReader::SeekSet);

	uint8_t **frames = new uint8_t *[nFrames];

	// Read frame
	for (int frame = 0; frame < nFrames; frame++)
	{
		data.Seek(12, FileReader::SeekCur);

		uint32_t chunks = data.ReadUInt32();

		uint8_t *frameBuffer = new uint8_t[imWidth * 4 * imHeight]{};

		for (uint32_t c = 0; c < chunks; c++)
		{
			uint32_t chunksize = data.ReadUInt32();
			uint16_t chunktype = data.ReadUInt16();

			if (chunktype == 0x2005)
			{
				// Skip the extra meta
				data.Seek(2, FileReader::SeekCur);

				int celOfX = data.ReadInt16();
				int celOfY = data.ReadInt16();

				data.Seek(1, FileReader::SeekCur);

				uint16_t celtype = data.ReadUInt16();
				assert(celtype == 2);

				data.Seek(7, FileReader::SeekCur);
				int celWidth  = data.ReadUInt16();
				int celheight = data.ReadUInt16();

				z_stream strm;
				// Read file into input buf

				size_t stream_length = chunksize - 6 - 7 - 9 - 4;

				uint8_t *in       = new uint8_t[stream_length];
				int      pixwidth = 4 * celWidth;
				uint8_t *out      = new uint8_t[pixwidth * celheight];
				data.Read(in, stream_length);

				strm.zalloc   = NULL;
				strm.zfree    = Z_NULL;
				strm.opaque   = Z_NULL;
				strm.avail_in = stream_length;
				strm.next_in  = (uint8_t *)in;
				int ret       = inflateInit(&strm);
				if (ret != Z_OK)
					perror("zlib");
				do
				{
					strm.avail_out = pixwidth * celheight;
					strm.next_out  = out;
					ret            = inflate(&strm, Z_NO_FLUSH);
					assert(ret != Z_STREAM_ERROR);
					switch (ret)
					{
					case Z_NEED_DICT:
						ret = Z_DATA_ERROR;
						/* falls through */
					case Z_DATA_ERROR:
					case Z_MEM_ERROR:
						inflateEnd(&strm);
						break;
					}
				} while (strm.avail_out == 0);

				inflateEnd(&strm);
				if (ret != Z_STREAM_END)
				{
					puts("NOOOOOOO");
				}
				else
				{
					// Blit onto the out thing
					for (int i = 0; i < celWidth * celheight; i++)
					{
						// Skip stuff out of range of the frame
						if ((i / celWidth) + celOfY >= imHeight || (i / celWidth) + celOfY < 0)
							continue;

						if ((i % celWidth) + celOfX < 0 || (i % celWidth) + celOfX >= imWidth)
							continue;

						// Skip fully transparent stuff. We can add blending later
						if (out[i * 4 + 3] == 0x00)
							continue;

						((uint32_t *)frameBuffer)[i % (celWidth) + celOfX + (i / celWidth + celOfY) * imWidth] =
							((uint32_t *)out)[i];
					}
				}
				delete[] in;
				delete[] out;
			}
			else
				data.Seek(chunksize - 6, FileReader::SeekCur);
		}

		frames[frame] = frameBuffer;
	}

	return new FASETexture(data, lumpnum, imWidth, imHeight, nFrames, frames);
}

//==========================================================================
//
//
//
//==========================================================================

FASETexture::~FASETexture()
{
	for (int i = 0; i < NumOfFrames; i++)
		delete[] framesData[i];

	delete[] framesData;
}

FASETexture::FASETexture(FileReader &lump, int lumpnum, int width, int height, int nFrames, uint8_t **frames)
	: FImageSource(lumpnum)
{
	Width  = width;
	Height = height;

	LeftOffset = width / 2;
	TopOffset  = height;

	framesData  = frames;
	NumOfFrames = nFrames;
}

void FASETexture::SetupPalette(FileReader &lump)
{
}

//==========================================================================
//
//
//
//==========================================================================

void FASETexture::ReadAlphaRemap(FileReader *lump, uint8_t *alpharemap)
{
}

//==========================================================================
//
//
//
//==========================================================================

PalettedPixels FASETexture::CreatePalettedPixels(int conversion, int frame)
{
	PalettedPixels Pixels(Width * Height);

	return Pixels;
}

//===========================================================================
//
// FASETexture::CopyPixels
//
//===========================================================================

int FASETexture::CopyPixels(FBitmap *bmp, int conversion, int frame)
{
	FileReader *lump;
	FileReader  lfr;

	lfr  = fileSystem.OpenFileReader(SourceLump);
	lump = &lfr;

	int pixwidth = Width * 4;

	// uint8_t *Pixels = new uint8_t[pixwidth * Height];

	// // memset(Pixels, 255, pixwidth * Height);
	// for (int i = 0; i < pixwidth * Height; i += 4)
	// {
	// 	Pixels[i]     = imageData[0 + i];
	// 	Pixels[i + 1] = imageData[1 + i];
	// 	Pixels[i + 2] = imageData[2 + i];
	// 	Pixels[i + 3] = imageData[3 + i];
	// }

	bmp->CopyPixelDataRGB(0, 0, framesData[frame], pixwidth, Height, 4, pixwidth, 0, CF_RGBA);

	// bmp->CopyPixelData(0,0,framesData[frame],pixwidth,Height,4,pixwidth,0)

	// memset(bmp->GetPixels(), 0x0, bmp->GetBufferSize());
	return -1;
}

#include "textures.h"

//==========================================================================
//
// A savegame picture
// This is essentially a stripped down version of the ASE texture
// only supporting the features actually present in a savegame
// that does not use an image source, because image sources are not
// meant to be transient data like the savegame picture.
//
//==========================================================================

class FASEFileTexture : public FTexture
{
  public:
	FASEFileTexture(FileReader &lump, int width, int height, uint8_t colortype);
	virtual FBitmap GetBgraBitmap(const PalEntry *remap, int *trans) override;

  protected:
	FileReader fr;
	uint8_t    ColorType;
	int        PaletteSize;
};

//==========================================================================
//
//
//
//==========================================================================

FGameTexture *ASETexture_CreateFromFile(PNGHandle *png, const FString &filename)
{
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FASEFileTexture::FASEFileTexture(FileReader &lump, int width, int height, uint8_t colortype) : ColorType(colortype)
{
	Width        = width;
	Height       = height;
	Masked       = false;
	bTranslucent = false;
	fr           = std::move(lump);
}

//===========================================================================
//
// FASETexture::CopyPixels
//
//===========================================================================

FBitmap FASEFileTexture::GetBgraBitmap(const PalEntry *remap, int *trans)
{
	FBitmap bmp;
	bmp.Create(Width, Height);
	bmp.Zero();
	return bmp;
}
