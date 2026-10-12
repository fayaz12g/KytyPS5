#ifndef KYTY_LOADER_GAME_ASPECT_RATIO_H_
#define KYTY_LOADER_GAME_ASPECT_RATIO_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace Loader::GamePatch {

struct AspectProfile {
	std::string_view title_id;
	std::string_view version;
	uint64_t instruction_offset;
	uint64_t padding_offset;
	std::array<uint8_t, 9> original;
	std::array<uint8_t, 5> replacement_prefix;
};

inline constexpr std::array<AspectProfile, 2> kAspectProfiles {{
    {"PPSA21564", "01.007.000", 0xFBF9F1, 0x73EB844,
     {0xC4, 0xE2, 0x79, 0x18, 0x87, 0x38, 0x02, 0x00, 0x00},
     {0xC4, 0xE2, 0x79, 0x18, 0x05}},
    {"PPSA01325", "01.905.000", 0x1619ABC, 0x1619A82,
     {0xC4, 0x62, 0x79, 0x18, 0x9F, 0xAC, 0x01, 0x00, 0x00},
     {0xC4, 0x62, 0x79, 0x18, 0x1D}},
}};

inline const AspectProfile* FindAspectProfile(std::string_view title_id,
                                             std::string_view version) {
	for (const auto& profile: kAspectProfiles) {
		if (profile.title_id == title_id && profile.version == version) {
			return &profile;
		}
	}
	return nullptr;
}

struct AspectMatch {
	std::optional<uint64_t> instruction_rva;
	bool ambiguous = false;
};

struct AspectPatchRange {
	uint64_t address;
	uint64_t size;
};

inline bool AspectRangesOverlap(AspectPatchRange left, AspectPatchRange right) {
	if (left.size == 0 || right.size == 0) {
		return false;
	}
	return left.address <= right.address ? right.address - left.address < left.size
	                                     : left.address - right.address < right.size;
}

inline bool ConflictsWithAspect(std::span<const AspectPatchRange> writes,
                                uint64_t instruction_address, uint64_t padding_address,
                                std::optional<uint64_t> live_value = std::nullopt) {
	return std::any_of(writes.begin(), writes.end(), [&](const AspectPatchRange& write) {
		return AspectRangesOverlap(write, {instruction_address, 9}) ||
		       AspectRangesOverlap(write, {padding_address, 4}) ||
		       (live_value && AspectRangesOverlap(write, {*live_value, 4}));
	});
}

inline AspectMatch FindAspectMatch(const AspectProfile& profile,
                                   std::span<const uint8_t> executable, uint64_t executable_rva,
                                   std::span<const uint8_t> image) {
	AspectMatch result;
	if (executable_rva > image.size() || executable.size() > image.size() - executable_rva) {
		return result;
	}
	for (auto current = executable.begin(); current != executable.end();) {
		const auto found = std::search(current, executable.end(), profile.original.begin(),
		                               profile.original.end());
		if (found == executable.end()) {
			break;
		}
		const auto rva = executable_rva + static_cast<uint64_t>(found - executable.begin());
		const bool forward = profile.padding_offset >= profile.instruction_offset;
		const auto distance = forward ? profile.padding_offset - profile.instruction_offset
		                              : profile.instruction_offset - profile.padding_offset;
		if ((forward && distance <= UINT64_MAX - rva) || (!forward && distance <= rva)) {
			const auto padding_rva = forward ? rva + distance : rva - distance;
			if (padding_rva <= image.size() && image.size() - padding_rva >= 4 &&
			    std::all_of(image.begin() + padding_rva, image.begin() + padding_rva + 4,
			                [](uint8_t byte) { return byte == 0xCC; })) {
				if (result.instruction_rva) {
					return {std::nullopt, true};
				}
				result.instruction_rva = rva;
			}
		}
		current = found + 1;
	}
	return result;
}

inline std::optional<std::array<uint8_t, 9>> MakeAspectInstruction(
    const AspectProfile& profile, uint64_t instruction_address, uint64_t value_address) {
	if (instruction_address > UINT64_MAX - profile.original.size()) {
		return std::nullopt;
	}
	const auto next = instruction_address + profile.original.size();
	const bool forward = value_address >= next;
	const auto distance = forward ? value_address - next : next - value_address;
	const auto limit = uint64_t {std::numeric_limits<int32_t>::max()} + (forward ? 0 : 1);
	if (distance > limit) {
		return std::nullopt;
	}
	const auto displacement = static_cast<int32_t>(forward ? static_cast<int64_t>(distance)
	                                                       : -static_cast<int64_t>(distance));
	std::array<uint8_t, 9> instruction {};
	for (size_t index = 0; index < profile.replacement_prefix.size(); ++index) {
		instruction[index] = profile.replacement_prefix[index];
	}
	std::memcpy(instruction.data() + profile.replacement_prefix.size(), &displacement,
	            sizeof(displacement));
	return instruction;
}

inline std::optional<float> WindowAspectRatio(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0) {
		return std::nullopt;
	}
	return static_cast<float>(width) / static_cast<float>(height);
}

inline bool StoreWindowAspect(uint32_t& storage, uint32_t width, uint32_t height) {
	static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
	const auto aspect = WindowAspectRatio(width, height);
	if (!aspect) {
		return false;
	}
	std::atomic_ref<uint32_t>(storage).store(std::bit_cast<uint32_t>(*aspect),
	                                       std::memory_order_relaxed);
	return true;
}

} // namespace Loader::GamePatch

#endif