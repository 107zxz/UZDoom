/*
** respritetexture.h
**
** Loading Resprite project archives as sprite frames.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

class FImageSource;
template<class T> class TArray;

bool RespriteTexture_Load(int wadnum, TArray<FImageSource*>& frames);
