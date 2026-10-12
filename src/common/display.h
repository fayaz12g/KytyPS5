#ifndef KYTY_COMMON_DISPLAY_H_
#define KYTY_COMMON_DISPLAY_H_

#include <algorithm>
#include <cstdint>

namespace Config {

enum class AspectRatio { Stretch, R16X9, R21X9, R32X9, R16X10, R4X3, Auto };

struct DisplaySize {
	uint32_t width;
	uint32_t height;
};

struct HostDisplay {
	int32_t     x;
	int32_t     y;
	DisplaySize size;
};

struct DisplayRect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

inline DisplaySize AspectReference(AspectRatio aspect, DisplaySize display = {1280, 720}) {
	uint32_t numerator   = 16;
	uint32_t denominator = 9;
	switch (aspect) {
		case AspectRatio::Auto:
			if (display.width != 0 && display.height != 0) {
				numerator   = display.width;
				denominator = display.height;
			}
			break;
		case AspectRatio::R21X9:
			if (display.height != 0 &&
			    uint64_t {display.width} * 10 >= uint64_t {display.height} * 23 &&
			    uint64_t {display.width} * 2 <= uint64_t {display.height} * 5) {
				numerator   = display.width;
				denominator = display.height;
			} else {
				numerator   = 43;
				denominator = 18;
			}
			break;
		case AspectRatio::R32X9: numerator = 32; break;
		case AspectRatio::R16X10: denominator = 10; break;
		case AspectRatio::R4X3:
			numerator   = 4;
			denominator = 3;
			break;
		default: break;
	}
	return {numerator, denominator};
}

inline DisplaySize AspectSize(AspectRatio aspect, uint32_t height,
                              DisplaySize display = {1280, 720}) {
	const auto reference = AspectReference(aspect, display);
	return {static_cast<uint32_t>((uint64_t {height} * reference.width + reference.height / 2) /
	                              reference.height),
	        height};
}

inline DisplayRect PresentationRect(AspectRatio aspect, DisplaySize surface,
                                    DisplaySize configured) {
	if (surface.width == 0 || surface.height == 0) {
		return {};
	}
	if (aspect == AspectRatio::Stretch) {
		return {0, 0, surface.width, surface.height};
	}
	const auto target = AspectReference(aspect, configured);
	if (target.width == 0 || target.height == 0) {
		return {0, 0, surface.width, surface.height};
	}
	auto size = surface;
	if (uint64_t {surface.width} * target.height > uint64_t {surface.height} * target.width) {
		size.width = std::clamp(
		    static_cast<uint32_t>((uint64_t {surface.height} * target.width + target.height / 2) /
			                      target.height),
		    1u, surface.width);
	} else {
		size.height = std::clamp(
		    static_cast<uint32_t>((uint64_t {surface.width} * target.height + target.width / 2) /
			                      target.width),
		    1u, surface.height);
	}
	return {(surface.width - size.width) / 2, (surface.height - size.height) / 2, size.width,
	        size.height};
}

} // namespace Config

#endif