/**
 * @file rgb.h
 * @brief RGB Pixel Type
 *
 * @details Defines the RGB pixel struct with 8-bit color channels,
 * used by display capture, image processing, and JPEG encoding.
 *
 * @ingroup core
 */

#pragma once

#include "core/types/primitives.h"

/// @brief RGB pixel with 8-bit color channels
struct RGB
{
	UINT8 Red;   
	UINT8 Green;
	UINT8 Blue;  
};


using PRGB = RGB *;
using PCRGB = const RGB *;
