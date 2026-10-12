#include "loader/gameAspectRatio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GameAspectRatioTests: %s\n", message);
		std::abort();
	}
}
}

int main() {
	using namespace Loader::GamePatch;
	const auto* bot = FindAspectProfile("PPSA21564", "01.007.000");
	const auto* playroom = FindAspectProfile("PPSA01325", "01.905.000");
	Check(bot != nullptr && playroom != nullptr, "supported profiles missing");
	Check(FindAspectProfile("PPSA21564", "01.008.000") == nullptr,
	      "unsupported version accepted");
	Check(FindAspectProfile("PPSA00000", "01.007.000") == nullptr,
	      "unsupported title accepted");
	Check(MakeAspectInstruction(*bot, bot->instruction_offset, bot->padding_offset) ==
	          std::array<uint8_t, 9> {0xC4, 0xE2, 0x79, 0x18, 0x05, 0x4A, 0xBE, 0x42, 0x06},
	      "ASTRO BOT encoding differs from supplied patch");
	Check(MakeAspectInstruction(*playroom, playroom->instruction_offset,
	                            playroom->padding_offset) ==
	          std::array<uint8_t, 9> {0xC4, 0x62, 0x79, 0x18, 0x1D, 0xBD, 0xFF, 0xFF, 0xFF},
	      "PLAYROOM encoding differs from supplied patch");
	Check(!MakeAspectInstruction(*bot, 0x1000, 0x100000000ULL),
	      "out-of-range displacement accepted");
	Check(!MakeAspectInstruction(*bot, UINT64_MAX, 0), "address overflow accepted");
	for (const auto& profile: kAspectProfiles) {
		const auto conflicts = [&](uint64_t address, uint64_t size) {
			const std::array<AspectPatchRange, 1> writes {{{address, size}}};
			return ConflictsWithAspect(writes, profile.instruction_offset,
			                           profile.padding_offset);
		};
		Check(conflicts(profile.instruction_offset, 9), "instruction patch conflict missed");
		Check(conflicts(profile.instruction_offset + 8, 1), "partial instruction overlap missed");
		Check(conflicts(profile.instruction_offset - 1, 2), "leading overlap missed");
		Check(conflicts(profile.padding_offset + 3, 1), "aspect data overlap missed");
		Check(!conflicts(profile.instruction_offset + 9, 1), "adjacent instruction write rejected");
		Check(!conflicts(profile.padding_offset + 4, 1), "adjacent data write rejected");
		Check(!conflicts(profile.instruction_offset, 0), "empty write rejected");
		Check(!conflicts(0x100, 4), "unrelated fix disables live aspect");
		const std::array<AspectPatchRange, 2> unrelated {{{0x100, 4}, {0x200, 9}}};
		Check(!ConflictsWithAspect(unrelated, profile.instruction_offset, profile.padding_offset),
		      "unrelated enabled fixes rejected");
		const std::array<AspectPatchRange, 0> disabled {};
		Check(!ConflictsWithAspect(disabled, profile.instruction_offset, profile.padding_offset),
		      "empty enabled-write list rejected");
	}
	const std::array<AspectPatchRange, 1> live_write {{{0x5002, 1}}};
	Check(ConflictsWithAspect(live_write, 0x1000, 0x2000, 0x5000),
	      "live float overlap missed");
	Check(!ConflictsWithAspect(live_write, 0x1000, 0x2000),
	      "unallocated live value treated as a conflict");
	Check(AspectRangesOverlap({UINT64_MAX - 2, 3}, {UINT64_MAX, 1}),
	      "high-address overlap lost to overflow");
	Check(!AspectRangesOverlap({UINT64_MAX - 2, 3}, {0, 1}),
	      "high-address range wrapped around");
	std::vector<uint8_t> image(0x400, 0);
	const auto place_playroom = [&](size_t site) {
		std::copy(playroom->original.begin(), playroom->original.end(), image.begin() + site);
		const auto padding = site - (playroom->instruction_offset - playroom->padding_offset);
		std::fill_n(image.begin() + padding, 4, 0xCC);
	};
	place_playroom(0x180);
	const auto executable = std::span<const uint8_t>(image).subspan(0x100, 0x200);
	auto match = FindAspectMatch(*playroom, executable, 0x100, image);
	Check(match.instruction_rva == 0x180 && !match.ambiguous,
	      "relocated signature was not found");
	Check(!FindAspectMatch(*bot, executable, 0x100, image).instruction_rva,
	      "wrong game's instruction accepted");
	image[0x146] = 0;
	Check(!FindAspectMatch(*playroom, executable, 0x100, image).instruction_rva,
	      "padding mismatch accepted");
	image[0x146] = 0xCC;
	image[0x180] = 0;
	Check(!FindAspectMatch(*playroom, executable, 0x100, image).instruction_rva,
	      "modified instruction accepted");
	image[0x180] = playroom->original.front();
	place_playroom(0x280);
	match = FindAspectMatch(*playroom, executable, 0x100, image);
	Check(match.ambiguous && !match.instruction_rva, "duplicate signatures accepted");
	Check(!FindAspectMatch(*playroom, executable, UINT64_MAX, image).instruction_rva,
	      "invalid segment range accepted");
	std::fill(image.begin(), image.end(), 0);
	place_playroom(0x180);
	Check(!FindAspectMatch(*playroom,
	                      std::span<const uint8_t>(image).subspan(0x180, 8), 0x180, image)
	           .instruction_rva,
	      "truncated instruction accepted");
	for (const auto& profile: kAspectProfiles) {
		const uint64_t site = 0x100000;
		for (const uint64_t value: {uint64_t {0x80000}, uint64_t {0x200000}}) {
			const auto instruction = MakeAspectInstruction(profile, site, value);
			Check(instruction.has_value(), "aligned live storage rejected");
			int32_t displacement = 0;
			std::memcpy(&displacement, instruction->data() + 5, sizeof(displacement));
			Check(static_cast<int64_t>(site + 9) + displacement == static_cast<int64_t>(value),
			      "instruction points to incorrect live storage");
		}
	}
	Check(!WindowAspectRatio(0, 1440) && !WindowAspectRatio(2560, 0),
	      "minimized window accepted");
	Check(std::abs(*WindowAspectRatio(1920, 1080) - 16.0f / 9.0f) < 0.000001f,
	      "initial aspect incorrect");
	Check(std::abs(*WindowAspectRatio(3440, 1440) - 43.0f / 18.0f) < 0.000001f,
	      "resized aspect incorrect");
	alignas(std::atomic_ref<uint32_t>::required_alignment) uint32_t storage = 0;
	Check(StoreWindowAspect(storage, 1920, 1080), "initial live write failed");
	Check(std::bit_cast<float>(storage) == *WindowAspectRatio(1920, 1080),
	      "guest float differs from initial window");
	Check(StoreWindowAspect(storage, 3024, 1964), "resize live write failed");
	Check(std::bit_cast<float>(storage) == *WindowAspectRatio(3024, 1964),
	      "guest float did not follow resize");
	const auto previous = storage;
	Check(!StoreWindowAspect(storage, 0, 0) && storage == previous,
	      "minimization overwrote live ratio");
	std::puts("Game aspect profiles, instruction encoding, and resize ratios passed.");
}