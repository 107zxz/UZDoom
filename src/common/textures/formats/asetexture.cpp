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
#include "miniz.h"
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
	FASETexture(int lumpnum, int width, int height, int nFrames, uint8_t **frameBuffers, uint8_t **palettedFrameBuffers,
	            bool masked, bool translucent);
	~FASETexture();

	int            CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override;
	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override;

  protected:
	uint8_t **framesData;
	uint8_t **framesPaletted;
};

namespace
{
enum
{
	ASE_OLD_PALETTE_CHUNK1 = 0x0004,
	ASE_OLD_PALETTE_CHUNK2 = 0x0011,
	ASE_CEL_CHUNK          = 0x2005,
	ASE_PALETTE_CHUNK      = 0x2019,
};

static bool ASEDecompress(FileReader &data, uint8_t *out, size_t outSize, size_t compressedSize)
{
	TArray<uint8_t> in;
	in.Resize((unsigned)compressedSize);
	if (compressedSize != 0 && size_t(data.Read(in.Data(), compressedSize)) != compressedSize)
	{
		return false;
	}

	z_stream strm  = {};
	strm.next_in   = in.Data();
	strm.avail_in  = (uInt)compressedSize;
	strm.next_out  = out;
	strm.avail_out = (uInt)outSize;

	int ret = inflateInit(&strm);
	if (ret != Z_OK)
	{
		return false;
	}

	ret = inflate(&strm, Z_FINISH);
	inflateEnd(&strm);
	return ret == Z_STREAM_END && strm.total_out == outSize;
}

static void ASESetPaletteColor(uint8_t palette[256][4], unsigned index, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                               uint8_t transparentIndex)
{
	if (index >= 256)
	{
		return;
	}
	palette[index][0] = r;
	palette[index][1] = g;
	palette[index][2] = b;
	palette[index][3] = index == transparentIndex ? 0 : a;
}

static void ASEReadOldPaletteChunk(FileReader &data, uint8_t palette[256][4], uint16_t chunkType,
                                   uint8_t transparentIndex)
{
	auto     packets = data.ReadUInt16();
	unsigned index   = 0;
	for (unsigned p = 0; p < packets; ++p)
	{
		index += data.ReadUInt8();
		unsigned colors = data.ReadUInt8();
		if (colors == 0)
		{
			colors = 256;
		}
		for (unsigned i = 0; i < colors; ++i, ++index)
		{
			auto r = data.ReadUInt8();
			auto g = data.ReadUInt8();
			auto b = data.ReadUInt8();
			if (chunkType == ASE_OLD_PALETTE_CHUNK1)
			{
				r = (uint8_t)((r * 255 + 31) / 63);
				g = (uint8_t)((g * 255 + 31) / 63);
				b = (uint8_t)((b * 255 + 31) / 63);
			}
			ASESetPaletteColor(palette, index, r, g, b, 255, transparentIndex);
		}
	}
}

static void ASEReadPaletteChunk(FileReader &data, uint8_t palette[256][4], uint8_t transparentIndex)
{
	data.ReadUInt32(); // palette size, not needed here.
	auto first = data.ReadUInt32();
	auto last  = data.ReadUInt32();
	data.Seek(8, FileReader::SeekCur);

	if (last < first)
	{
		return;
	}

	for (uint32_t index = first; index <= last; ++index)
	{
		auto flags = data.ReadUInt16();
		auto r     = data.ReadUInt8();
		auto g     = data.ReadUInt8();
		auto b     = data.ReadUInt8();
		auto a     = data.ReadUInt8();
		ASESetPaletteColor(palette, index, r, g, b, a, transparentIndex);
		if ((flags & 1) != 0)
		{
			auto nameLength = data.ReadUInt16();
			data.Seek(nameLength, FileReader::SeekCur);
		}
	}
}
} // namespace

//==========================================================================
//
//
//
//==========================================================================

FImageSource *ASEImage_TryCreate(FileReader &data, int lumpnum)
{
	if ((size_t)data.GetLength() < 128)
	{
		return nullptr;
	}

	data.Seek(0, FileReader::SeekSet);
	data.ReadUInt32();
	uint16_t magic = data.ReadUInt16();
	if (magic != 0xa5e0)
	{
		return nullptr;
	}

	int      nFrames    = data.ReadUInt16();
	int      imWidth    = data.ReadUInt16();
	int      imHeight   = data.ReadUInt16();
	uint16_t colorDepth = data.ReadUInt16();
	if (nFrames <= 0 || imWidth <= 0 || imHeight <= 0)
	{
		return nullptr;
	}
	if (colorDepth != 8 && colorDepth != 16 && colorDepth != 32)
	{
		return nullptr;
	}

	data.Seek(28, FileReader::SeekSet);
	uint8_t transparentIndex = data.ReadUInt8();
	data.Seek(128, FileReader::SeekSet);

	uint8_t palette[256][4];
	for (int i = 0; i < 256; ++i)
	{
		palette[i][0] = (uint8_t)i;
		palette[i][1] = (uint8_t)i;
		palette[i][2] = (uint8_t)i;
		palette[i][3] = i == transparentIndex ? 0 : 255;
	}

	uint8_t **frames         = new uint8_t *[nFrames];
	uint8_t **palettedFrames = colorDepth == 8 ? new uint8_t *[nFrames] : nullptr;
	bool      hasMasked      = false;
	bool      hasTranslucent = false;
	int       pixelSize      = colorDepth == 32 ? 4 : colorDepth == 16 ? 2 : 1;

	for (int frame = 0; frame < nFrames; ++frame)
	{
		auto frameStart = data.Tell();
		auto frameBytes = data.ReadUInt32();
		auto frameMagic = data.ReadUInt16();
		if (frameMagic != 0xf1fa || frameBytes < 16)
		{
			return nullptr;
		}

		auto oldChunks = data.ReadUInt16();
		data.Seek(4, FileReader::SeekCur);
		auto chunks = data.ReadUInt32();
		if (chunks == 0)
		{
			chunks = oldChunks;
		}

		auto frameDataStart = data.Tell();
		auto frameEnd       = frameStart + frameBytes;

		if (colorDepth == 8)
		{
			for (uint32_t c = 0; c < chunks && data.Tell() < frameEnd; ++c)
			{
				auto chunkStart = data.Tell();
				auto chunkSize  = data.ReadUInt32();
				auto chunkType  = data.ReadUInt16();
				if (chunkSize < 6)
				{
					return nullptr;
				}

				switch (chunkType)
				{
				case ASE_OLD_PALETTE_CHUNK1:
				case ASE_OLD_PALETTE_CHUNK2:
					ASEReadOldPaletteChunk(data, palette, chunkType, transparentIndex);
					break;

				case ASE_PALETTE_CHUNK:
					ASEReadPaletteChunk(data, palette, transparentIndex);
					break;
				}
				data.Seek(chunkStart + chunkSize, FileReader::SeekSet);
			}
		}

		uint8_t paletteMap[256] = {};
		if (colorDepth == 8)
		{
			for (int i = 0; i < 256; ++i)
			{
				paletteMap[i] = ImageHelpers::RGBToPalette(false, palette[i][0], palette[i][1], palette[i][2], palette[i][3]);
			}
		}

		uint8_t *frameBuffer         = new uint8_t[imWidth * imHeight * 4]{};
		uint8_t *framePalettedBuffer = colorDepth == 8 ? new uint8_t[imWidth * imHeight]{} : nullptr;
		data.Seek(frameDataStart, FileReader::SeekSet);
		for (uint32_t c = 0; c < chunks && data.Tell() < frameEnd; ++c)
		{
			auto chunkStart = data.Tell();
			auto chunkSize  = data.ReadUInt32();
			auto chunkType  = data.ReadUInt16();
			if (chunkSize < 6)
			{
				return nullptr;
			}

			if (chunkType == ASE_CEL_CHUNK)
			{
				auto chunkEnd = chunkStart + chunkSize;
				data.ReadUInt16(); // layer index
				auto celOfX     = data.ReadInt16();
				auto celOfY     = data.ReadInt16();
				auto celOpacity = data.ReadUInt8();
				auto celType    = data.ReadUInt16();
				data.Seek(7, FileReader::SeekCur);

				if (celType == 0 || celType == 2)
				{
					auto            celWidth  = data.ReadUInt16();
					auto            celHeight = data.ReadUInt16();
					size_t          rawSize   = size_t(celWidth) * celHeight * pixelSize;
					TArray<uint8_t> celPixels;
					celPixels.Resize((unsigned)rawSize);

					bool ok = false;
					if (celType == 0)
					{
						ok = rawSize == 0 || size_t(data.Read(celPixels.Data(), rawSize)) == rawSize;
					}
					else if (data.Tell() <= chunkEnd)
					{
						auto compressedSize = size_t(chunkEnd - data.Tell());
						ok                  = ASEDecompress(data, celPixels.Data(), rawSize, compressedSize);
					}

					if (!ok)
					{
						return nullptr;
					}

					for (int y = 0; y < celHeight; ++y)
					{
						for (int x = 0; x < celWidth; ++x)
						{
							int dstX = x + celOfX;
							int dstY = y + celOfY;
							if (dstX < 0 || dstX >= imWidth || dstY < 0 || dstY >= imHeight)
							{
								continue;
							}

							size_t  srcPos = (size_t(y) * celWidth + x) * pixelSize;
							uint8_t r, g, b, a;
							if (colorDepth == 32)
							{
								r = celPixels[srcPos + 0];
								g = celPixels[srcPos + 1];
								b = celPixels[srcPos + 2];
								a = celPixels[srcPos + 3];
							}
							else if (colorDepth == 16)
							{
								r = g = b = celPixels[srcPos + 0];
								a         = celPixels[srcPos + 1];
							}
							else
							{
								auto index = celPixels[srcPos];
								r          = palette[index][0];
								g          = palette[index][1];
								b          = palette[index][2];
								a          = palette[index][3];
							}

							if (celOpacity != 255)
							{
								a = (uint8_t)((unsigned(a) * celOpacity + 127) / 255);
							}
							if (a == 0)
							{
								hasMasked = true;
								continue;
							}
							if (a != 255)
							{
								hasTranslucent = true;
							}

							auto dstPixel = size_t(dstY) * imWidth + dstX;
							auto dstPos   = dstPixel * 4;
							frameBuffer[dstPos + 0] = r;
							frameBuffer[dstPos + 1] = g;
							frameBuffer[dstPos + 2] = b;
							frameBuffer[dstPos + 3] = a;
							if (framePalettedBuffer != nullptr)
							{
								framePalettedBuffer[dstPixel] = paletteMap[celPixels[srcPos]];
							}
						}
					}
				}
			}

			data.Seek(chunkStart + chunkSize, FileReader::SeekSet);
		}

		if (!hasMasked)
		{
			for (int i = 0; i < imWidth * imHeight; ++i)
			{
				if (frameBuffer[i * 4 + 3] == 0)
				{
					hasMasked = true;
					break;
				}
			}
		}

		frames[frame] = frameBuffer;
		if (palettedFrames != nullptr)
		{
			palettedFrames[frame] = framePalettedBuffer;
		}
		data.Seek(frameEnd, FileReader::SeekSet);
	}

	return new FASETexture(lumpnum, imWidth, imHeight, nFrames, frames, palettedFrames, hasMasked, hasTranslucent);
}

//==========================================================================
//
//
//
//==========================================================================

FASETexture::~FASETexture()
{
	// for (int i = 0; i < NumOfFrames; i++)
	// 	delete[] framesData[i];

	// delete[] framesData;
}

FASETexture::FASETexture(int lumpnum, int width, int height, int nFrames, uint8_t **frames, uint8_t **palettedFrames,
                         bool masked, bool translucent)
	: FImageSource(lumpnum)
{
	Width  = width;
	Height = height;

	LeftOffset = width / 2;
	TopOffset  = height;

	framesData      = frames;
	framesPaletted  = palettedFrames;
	NumOfFrames     = nFrames;
	bMasked         = masked;
	bTranslucent    = translucent;
}

//==========================================================================
//
//
//
//==========================================================================

PalettedPixels FASETexture::CreatePalettedPixels(int conversion, int frame)
{
	if (frame < 0 || frame >= NumOfFrames)
	{
		frame = 0;
	}

	if (framesPaletted != nullptr && conversion != luminance)
	{
		PalettedPixels pixels(Width * Height);
		ImageHelpers::FlipNonSquareBlock(pixels.Data(), framesPaletted[frame], Width, Height, Width);
		return pixels;
	}

	PalettedPixels pixels(Width * Height);
	uint8_t       *dest    = pixels.Data();
	const uint8_t *src     = framesData[frame];
	bool           doalpha = conversion == luminance;
	int            destAdv = Height;
	int            destRew = Width * Height - 1;

	for (int y = Height; y != 0; --y)
	{
		for (int x = Width; x != 0; --x)
		{
			auto r = *src++;
			auto g = *src++;
			auto b = *src++;
			auto a = *src++;
			*dest  = ImageHelpers::RGBToPalette(doalpha, r, g, b, a);
			dest += destAdv;
		}
		dest -= destRew;
	}

	return pixels;
}

//===========================================================================
//
// FASETexture::CopyPixels
//
//===========================================================================

int FASETexture::CopyPixels(FBitmap *bmp, int conversion, int frame)
{
	if (frame < 0 || frame >= NumOfFrames)
	{
		frame = 0;
	}

	bmp->CopyPixelDataRGB(0, 0, framesData[frame], Width, Height, 4, Width * 4, 0, CF_RGBA);
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
