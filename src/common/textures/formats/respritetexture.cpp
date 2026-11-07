/*
** respritetexture.cpp
**
** Loading Resprite project archives as sprite frames.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "bitmap.h"
#include "filesystem.h"
#include "image.h"
#include "imagehelpers.h"
#include "memarena.h"
#include "printf.h"
#include "common/engine/serializer_rapidjson.h"
#include "respritetexture.h"

using namespace FileSys;

namespace
{
constexpr int MaxRespriteDimension = 8192;
constexpr int MaxRespriteFrames = 29;
constexpr size_t MaxRespritePixels = 64 * 1024 * 1024;
constexpr uint64_t MaxRespriteDecodedBytes = 256ull * 1024 * 1024;

class FRespriteImage final : public FImageSource
{
public:
	FRespriteImage(int width, int height, const uint8_t* pixels, bool masked, bool translucent)
		: Pixels(pixels)
	{
		Width = width;
		Height = height;
		LeftOffset = width / 2;
		TopOffset = height;
		bMasked = masked;
		bTranslucent = translucent;
	}

	int CopyPixels(FBitmap* bitmap, int, int = 0) override
	{
		bitmap->CopyPixelDataRGB(0, 0, Pixels, Width, Height, 4, Width * 4, 0, CF_BGRA);
		return bTranslucent ? -1 : (bMasked ? 1 : 0);
	}

	PalettedPixels CreatePalettedPixels(int conversion, int = 0) override
	{
		PalettedPixels result(Width * Height);
		const bool doLuminance = conversion == FImageSource::luminance;
		for (int x = 0; x < Width; ++x)
		{
			for (int y = 0; y < Height; ++y)
			{
				const auto* pixel = Pixels + (size_t(y) * Width + x) * 4;
				result[x * Height + y] = ImageHelpers::RGBToPalette(doLuminance, pixel[2], pixel[1], pixel[0], pixel[3]);
			}
		}
		return result;
	}

private:
	const uint8_t* Pixels;
};

bool ReadInt(const rapidjson::Value& object, const char* key, int& value)
{
	if (!object.IsObject()) return false;
	auto it = object.FindMember(key);
	if (it == object.MemberEnd() || !it->value.IsInt()) return false;
	value = it->value.GetInt();
	return true;
}

bool IsUuid(const char* id)
{
	if (!id || std::strlen(id) != 36) return false;
	for (int i = 0; i < 36; ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (id[i] != '-') return false;
		}
		else if (!std::isxdigit(static_cast<unsigned char>(id[i])))
			return false;
	}
	return true;
}

int FindDocument(int wadnum, std::string& prefix)
{
	for (int lump = fileSystem.GetFirstEntry(wadnum); lump <= fileSystem.GetLastEntry(wadnum); ++lump)
	{
		const char* name = fileSystem.GetFileFullName(lump);
		if (!name) continue;
		const char* base = std::strrchr(name, '/');
		base = base ? base + 1 : name;
		if (stricmp(base, "document.json") == 0)
		{
			prefix.assign(name, size_t(base - name));
			return lump;
		}
	}
	return -1;
}

uint8_t GetOpacity(const rapidjson::Value& object, const char* key)
{
	if (!object.IsObject()) return 255;
	auto value = object.FindMember(key);
	if (value == object.MemberEnd() || !value->value.IsNumber()) return 255;
	const double opacity = value->value.GetDouble();
	if (!std::isfinite(opacity)) return 255;
	return static_cast<uint8_t>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255.0));
}

void CompositeCell(uint8_t* canvas, int canvasWidth, int canvasHeight, int originX, int originY,
	FImageSource* image, uint8_t opacity)
{
	if (!image || opacity == 0) return;

	const int width = image->GetWidth();
	const int height = image->GetHeight();
	if (width <= 0 || height <= 0) return;

	FBitmap bitmap;
	bitmap.Create(width, height);
	bitmap.Zero();
	image->CopyPixels(&bitmap, FImageSource::normal);
	const uint8_t* source = bitmap.GetPixels();

	const int x0 = std::max(0, originX);
	const int y0 = std::max(0, originY);
	const int x1 = std::min(canvasWidth, originX + width);
	const int y1 = std::min(canvasHeight, originY + height);
	for (int y = y0; y < y1; ++y)
	{
		for (int x = x0; x < x1; ++x)
		{
			const auto* src = source + (size_t(y - originY) * width + (x - originX)) * 4;
			auto* dst = canvas + (size_t(y) * canvasWidth + x) * 4;
			const unsigned sourceAlpha = (unsigned(src[3]) * opacity + 127) / 255;
			if (sourceAlpha == 0) continue;

			const unsigned destAlpha = dst[3];
			const unsigned outAlpha = sourceAlpha + (destAlpha * (255 - sourceAlpha) + 127) / 255;
			for (int channel = 0; channel < 3; ++channel)
			{
				const unsigned sourceColor = (unsigned(src[channel]) * sourceAlpha);
				const unsigned destColor = (unsigned(dst[channel]) * destAlpha * (255 - sourceAlpha) + 127) / 255;
				dst[channel] = outAlpha ? static_cast<uint8_t>((sourceColor + destColor + outAlpha / 2) / outAlpha) : 0;
			}
			dst[3] = static_cast<uint8_t>(outAlpha);
		}
	}
}

bool LoadRespriteDocument(int wadnum, int documentLump, const std::string& prefix, TArray<FImageSource*>& frames)
{
	if (fileSystem.FileLength(documentLump) <= 0 || fileSystem.FileLength(documentLump) > 16 * 1024 * 1024)
	{
		Printf(TEXTCOLOR_YELLOW "WARNING: Resprite document is too large or empty in %s\n", fileSystem.GetResourceFileName(wadnum));
		return false;
	}
	auto jsonData = fileSystem.ReadFile(documentLump);
	rapidjson::Document document;
	document.Parse(reinterpret_cast<const char*>(jsonData.bytes()), jsonData.size());
	if (document.HasParseError() || !document.IsObject())
	{
		Printf(TEXTCOLOR_YELLOW "WARNING: Invalid Resprite document in %s\n", fileSystem.GetResourceFileName(wadnum));
		return false;
	}

	int width = 0, height = 0, frameCount = 0;
	const auto canvas = document.FindMember("canvasSize");
	if (canvas == document.MemberEnd() || !ReadInt(canvas->value, "width", width) || !ReadInt(canvas->value, "height", height) ||
		!ReadInt(document, "frameCount", frameCount) || width <= 0 || height <= 0 || width > MaxRespriteDimension ||
		height > MaxRespriteDimension || size_t(width) * size_t(height) > MaxRespritePixels || frameCount <= 0 ||
		frameCount > MaxRespriteFrames || uint64_t(width) * height * 4 * frameCount > MaxRespriteDecodedBytes)
	{
		Printf(TEXTCOLOR_YELLOW "WARNING: Unsupported Resprite dimensions or frame count in %s\n", fileSystem.GetResourceFileName(wadnum));
		return false;
	}

	auto layers = document.FindMember("layerDatas");
	if (layers == document.MemberEnd() || !layers->value.IsArray() || layers->value.Size() > 1024)
	{
		Printf(TEXTCOLOR_YELLOW "WARNING: Resprite document has no layer data in %s\n", fileSystem.GetResourceFileName(wadnum));
		return false;
	}

	struct FInheritedCell
	{
		std::string CellId;
		int BaseX = 0;
		int BaseY = 0;
		int X = 0;
		int Y = 0;
		uint8_t Opacity = 255;
		bool HasSource = false;
	};
	std::vector<FInheritedCell> inheritedCells(layers->value.Size());

	// Resprite stores the topmost layer first; composite bottom-to-top.
	for (int frame = 0; frame < frameCount; ++frame)
	{
		auto* canvasPixels = static_cast<uint8_t*>(ImageArena.Alloc(size_t(width) * height * 4));
		std::memset(canvasPixels, 0, size_t(width) * height * 4);
		bool masked = false;
		bool translucent = false;

		for (size_t layerIndex = layers->value.Size(); layerIndex > 0; --layerIndex)
		{
			const size_t index = layerIndex - 1;
			const auto& layer = layers->value[index];
			if (!layer.IsObject()) continue;

			auto group = layer.FindMember("isGroup");
			if (group != layer.MemberEnd() && group->value.IsBool() && group->value.GetBool())
				continue; // Group entries are organizational; their raster child layers are stored separately.
			auto kind = layer.FindMember("layerKind");
			if (kind != layer.MemberEnd() && kind->value.IsString() && std::strcmp(kind->value.GetString(), "raster") != 0)
				continue;

			auto blend = layer.FindMember("blendMode");
			if (blend != layer.MemberEnd() && blend->value.IsString() && std::strcmp(blend->value.GetString(), "bm-normal") != 0)
			{
				Printf(TEXTCOLOR_YELLOW "WARNING: Resprite blend modes other than normal are not supported in %s\n", fileSystem.GetResourceFileName(wadnum));
				return false;
			}

			auto cells = layer.FindMember("cells");
			if (cells == layer.MemberEnd() || !cells->value.IsArray() || static_cast<size_t>(frame) >= cells->value.Size())
			{
				inheritedCells[index].HasSource = false;
				continue;
			}
			const auto& cell = cells->value[frame];
			if (!cell.IsObject())
			{
				inheritedCells[index].HasSource = false;
				continue;
			}

			FInheritedCell& inherited = inheritedCells[index];
			auto inherit = cell.FindMember("inherit");
			const bool isInherited = inherit != cell.MemberEnd() && inherit->value.IsBool() && inherit->value.GetBool();
			int x = 0, y = 0;
			if (isInherited)
			{
				if (!inherited.HasSource) continue;

				// An inherited cell reuses the previous drawable and position. Its offset is
				// relative to that drawable's original position; bounds describe no new position.
				x = inherited.X;
				y = inherited.Y;
				auto offset = cell.FindMember("inheritOffset");
				if (offset != cell.MemberEnd() && offset->value.IsObject())
				{
					int offsetX = 0, offsetY = 0;
					if (ReadInt(offset->value, "x", offsetX) && ReadInt(offset->value, "y", offsetY))
					{
						x = inherited.BaseX + offsetX;
						y = inherited.BaseY + offsetY;
					}
				}

				inherited.X = x;
				inherited.Y = y;
			}
			else
			{
				auto id = cell.FindMember("id");
				auto bounds = cell.FindMember("bounds");
				auto origin = bounds != cell.MemberEnd() && bounds->value.IsObject() ? bounds->value.FindMember("origin") : rapidjson::Value::ConstMemberIterator();
				if (id == cell.MemberEnd() || !id->value.IsString() || !IsUuid(id->value.GetString()) ||
					bounds == cell.MemberEnd() || !bounds->value.IsObject() || origin == bounds->value.MemberEnd() ||
					!ReadInt(origin->value, "x", x) || !ReadInt(origin->value, "y", y))
				{
					inherited.HasSource = false;
					continue;
				}
				inherited.CellId = id->value.GetString();
				inherited.BaseX = inherited.X = x;
				inherited.BaseY = inherited.Y = y;
				inherited.HasSource = true;
			}

			auto cellOpacityMember = cell.FindMember("opacity");
			if (!isInherited || cellOpacityMember != cell.MemberEnd())
				inherited.Opacity = GetOpacity(cell, "opacity");

			auto visible = layer.FindMember("contentVisible");
			if (visible != layer.MemberEnd() && visible->value.IsBool() && !visible->value.GetBool()) continue;

			std::string cellPath = prefix + "CellData/" + inherited.CellId + ".png";
			int cellLump = fileSystem.CheckNumForFullName(cellPath.c_str(), wadnum);
			if (cellLump < 0) continue;
			auto image = FImageSource::GetImage(cellLump, false);
			if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0 || image->GetWidth() > MaxRespriteDimension ||
				image->GetHeight() > MaxRespriteDimension || size_t(image->GetWidth()) * image->GetHeight() > MaxRespritePixels)
				continue;

			uint8_t opacity = GetOpacity(layer, "opacity");
			opacity = static_cast<uint8_t>((unsigned(opacity) * inherited.Opacity + 127) / 255);
			CompositeCell(canvasPixels, width, height, x, y, image, opacity);
		}

		const size_t pixelCount = size_t(width) * height;
		for (size_t pixel = 0; pixel < pixelCount; ++pixel)
		{
			const uint8_t alpha = canvasPixels[pixel * 4 + 3];
			masked = masked || alpha < 255;
			translucent = translucent || (alpha != 0 && alpha != 255);
		}
		frames.Push(new FRespriteImage(width, height, canvasPixels, masked, translucent));
	}
	return true;
}
}

bool RespriteTexture_Load(int wadnum, TArray<FImageSource*>& frames)
{
	std::string prefix;
	const int documentLump = FindDocument(wadnum, prefix);
	if (documentLump < 0) return false;
	return LoadRespriteDocument(wadnum, documentLump, prefix, frames);
}
