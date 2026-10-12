#include "loader/gamePatch.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/elf.h"
#include "loader/gameAspectRatio.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Loader::GamePatch {

namespace {

constexpr uint64_t kPageSize = 0x4000;

struct Write {
	uint64_t             source_address = 0;
	uint64_t             target_address = 0;
	std::string          mod_name;
	std::vector<uint8_t> off;
	std::vector<uint8_t> on;
};

struct Plan {
	std::string              title_id;
	std::string              version;
	std::string              process;
	std::vector<Write>       writes;
	std::vector<std::string> mod_names;
	std::vector<uint64_t>    cave_pages;
};

using Json = nlohmann::json;

std::unique_ptr<Plan> g_pending_plan;
std::vector<uint64_t> g_applied_cave_pages;
std::mutex g_aspect_mutex;
const AspectProfile* g_aspect_profile = nullptr;
bool g_aspect_pending = false;
Program* g_aspect_program = nullptr;
uint64_t g_aspect_page = 0;
uint64_t g_aspect_instruction_address = 0;
uint64_t g_aspect_padding_address = 0;
uint32_t g_window_width = 0;
uint32_t g_window_height = 0;

bool Fail(std::string* error, std::string message) {
	*error = std::move(message);
	return false;
}

bool ParseBytes(const std::string& text, std::vector<uint8_t>* bytes) {
	if (text.empty() || (text.size() % 2) != 0) {
		return false;
	}

	bytes->resize(text.size() / 2);
	for (size_t index = 0; index < bytes->size(); index++) {
		(*bytes)[index] = static_cast<uint8_t>(std::stoul(text.substr(index * 2, 2), nullptr, 16));
	}
	return true;
}

bool LoadPlan(const std::filesystem::path& path, Plan* plan, std::string* error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Fail(error, "could not read cheat file");
	}

	const auto root = Json::parse(file, nullptr, false);
	if (!root.is_object() || !root.contains("id") || !root.contains("version") ||
	    !root.contains("process") || !root.contains("mods")) {
		return Fail(error, "expected a GoldHEN-style mods JSON file");
	}
	plan->title_id = root["id"].get<std::string>();
	plan->version  = root["version"].get<std::string>();
	plan->process  = root["process"].get<std::string>();

	for (const auto& mod: root["mods"]) {
		if (!mod.value("enabled", true)) {
			continue;
		}
		const auto name = mod["name"].get<std::string>();
		plan->mod_names.push_back(name);
		for (const auto& entry: mod["memory"]) {
			Write write;
			write.mod_name = name;
			write.source_address = std::stoull(entry["offset"].get<std::string>(), nullptr, 16);
			if (!ParseBytes(entry["off"].get<std::string>(), &write.off) ||
			    !ParseBytes(entry["on"].get<std::string>(), &write.on) ||
			    write.off.size() != write.on.size()) {
				return Fail(error, "invalid mod memory bytes");
			}
			plan->writes.push_back(std::move(write));
		}
	}
	return true;
}

void SuppressAspectWrites(Plan* plan, bool translated) {
	if (!Config::WindowAspectEnabled()) {
		return;
	}
	std::lock_guard lock(g_aspect_mutex);
	const AspectProfile* profile = nullptr;
	if (translated) {
		if (g_aspect_page == 0) {
			return;
		}
		profile = g_aspect_profile;
	} else {
		for (const auto& candidate: kAspectProfiles) {
			if (Common::EqualNoCase(plan->title_id, std::string(candidate.title_id)) &&
			    plan->version == candidate.version &&
			    Common::EqualNoCase(plan->process, "eboot.bin")) {
				profile = &candidate;
				break;
			}
		}
	}
	if (profile == nullptr) {
		return;
	}
	const auto instruction = translated ? g_aspect_instruction_address : profile->instruction_offset;
	const auto padding = translated ? g_aspect_padding_address : profile->padding_offset;
	const auto live_value = translated ? std::optional<uint64_t>(g_aspect_page) : std::nullopt;
	std::erase_if(plan->writes, [&](const Write& write) {
		const auto address = translated ? write.target_address : write.source_address;
		const std::array<AspectPatchRange, 1> range {{{address, write.on.size()}}};
		if (!ConflictsWithAspect(range, instruction, padding, live_value)) {
			return false;
		}
		::printf("Game cheat: skipped write from %s at 0x%llx (window aspect takes precedence)\n",
		         write.mod_name.c_str(), static_cast<unsigned long long>(write.source_address));
		return true;
	});
	std::erase_if(plan->mod_names, [&](const std::string& name) {
		return std::none_of(plan->writes.begin(), plan->writes.end(),
		                    [&](const Write& write) { return write.mod_name == name; });
	});
}

bool ValidateGame(const Plan& plan, const Program* main_program, std::string* error) {
	if (main_program == nullptr || main_program->base_vaddr == 0) {
		return Fail(error, "main executable is not loaded");
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) ||
	    !Common::EqualNoCase(main_program->file_name.filename().string(), plan.process) ||
	    !Common::EqualNoCase(title_id, plan.title_id) || version != plan.version) {
		return Fail(error, "cheat file does not match the loaded game");
	}
	return true;
}

void Translate(const Program& program, uint64_t source_base, uint64_t source_address,
               uint64_t* target_address) {
	if (source_address >= source_base) {
		*target_address = program.base_vaddr + source_address - source_base;
		return;
	}
	*target_address = program.base_vaddr - (source_base - source_address);
}

bool IsInsideProgram(const Program& program, uint64_t address, size_t size) {
	return size != 0 && address >= program.base_vaddr &&
	       address + size <= program.base_vaddr + program.mapped_size;
}

bool IsZero(const std::vector<uint8_t>& bytes) {
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

bool TranslateAndValidate(Plan* plan, const Program& program, uint64_t source_base) {
	for (auto& write: plan->writes) {
		Translate(program, source_base, write.source_address, &write.target_address);
		if (!IsZero(write.off) &&
		    (!IsInsideProgram(program, write.target_address, write.off.size()) ||
		     std::memcmp(reinterpret_cast<const void*>(write.target_address), write.off.data(),
		                 write.off.size()) != 0)) {
			return false;
		}
	}
	return true;
}

bool Matches(Plan* plan, const Program& program, uint64_t* source_base) {
	const auto anchor = std::find_if(plan->writes.begin(), plan->writes.end(),
	                                 [](const Write& write) { return !IsZero(write.off); });
	if (anchor == plan->writes.end() || program.mapped_size < anchor->off.size()) {
		return false;
	}

	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    segment.p_filesz < anchor->off.size()) {
			continue;
		}

		const auto* begin = reinterpret_cast<const uint8_t*>(program.base_vaddr + segment.p_vaddr);
		const auto* end   = begin + segment.p_filesz;
		for (auto* current = begin; current < end;) {
			const auto* found = std::search(current, end, anchor->off.begin(), anchor->off.end());
			if (found == end) {
				break;
			}
			const auto rva = static_cast<uint64_t>(found - begin) + segment.p_vaddr;
			if (anchor->source_address >= rva) {
				*source_base = anchor->source_address - rva;
				if (TranslateAndValidate(plan, program, *source_base)) {
					return true;
				}
			}
			current = found + 1;
		}
	}
	return false;
}

void ApplyPendingAspect(Program* program) {
	std::lock_guard lock(g_aspect_mutex);
	if (!g_aspect_pending || program == nullptr || program->elf == nullptr ||
	    !Common::EqualNoCase(program->file_name.filename().string(), "eboot.bin")) {
		return;
	}
	std::optional<uint64_t> instruction_rva;
	const std::span<const uint8_t> image {
	    reinterpret_cast<const uint8_t*>(program->base_vaddr), program->mapped_size};
	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	for (Elf64_Half index = 0; index < ehdr->e_phnum; ++index) {
		const auto& segment = phdr[index];
		if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    (segment.p_flags & PF_X) == 0 || segment.p_vaddr > image.size() ||
		    segment.p_filesz > image.size() - segment.p_vaddr) {
			continue;
		}
		const auto match = FindAspectMatch(*g_aspect_profile,
		                                  image.subspan(segment.p_vaddr, segment.p_filesz),
		                                  segment.p_vaddr, image);
		if (match.ambiguous || (match.instruction_rva && instruction_rva &&
		                        *match.instruction_rva != *instruction_rva)) {
			::printf("Window aspect: skipped (ambiguous instruction signature)\n");
			g_aspect_pending = false;
			return;
		}
		if (match.instruction_rva) {
			instruction_rva = match.instruction_rva;
		}
	}
	if (!instruction_rva) {
		return;
	}
	const auto instruction_address = program->base_vaddr + *instruction_rva;
	const auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    program->base_vaddr + program->mapped_size, kPageSize,
	    Common::VirtualMemory::Mode::ReadWrite, "game_window_aspect", false);
	const auto instruction = MakeAspectInstruction(*g_aspect_profile, instruction_address, page);
	if (page == 0 || !instruction) {
		if (page != 0) {
			EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(page, kPageSize));
		}
		::printf("Window aspect: skipped (no reachable guest storage)\n");
		g_aspect_pending = false;
		return;
	}
	auto* storage = new (reinterpret_cast<void*>(page)) uint32_t {};
	StoreWindowAspect(*storage, g_window_width, g_window_height);
	std::memcpy(reinterpret_cast<void*>(instruction_address), instruction->data(),
	            instruction->size());
	if (!Common::VirtualMemory::FlushInstructionCache(instruction_address, instruction->size())) {
		std::memcpy(reinterpret_cast<void*>(instruction_address), g_aspect_profile->original.data(),
		            g_aspect_profile->original.size());
		EXIT_IF(!Common::VirtualMemory::FlushInstructionCache(
		    instruction_address, g_aspect_profile->original.size()));
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(page, kPageSize));
		::printf("Window aspect: skipped (could not finalize instruction)\n");
		g_aspect_pending = false;
		return;
	}
	g_aspect_program = program;
	g_aspect_page = page;
	g_aspect_instruction_address = instruction_address;
	g_aspect_padding_address = g_aspect_profile->padding_offset >= g_aspect_profile->instruction_offset
	    ? instruction_address + (g_aspect_profile->padding_offset - g_aspect_profile->instruction_offset)
	    : instruction_address - (g_aspect_profile->instruction_offset - g_aspect_profile->padding_offset);
	::printf("Window aspect: enabled for %s %s, initial ratio %.6f\n",
	         g_aspect_profile->title_id.data(), g_aspect_profile->version.data(),
	         std::bit_cast<float>(*storage));
	g_aspect_pending = false;
}

void ReleaseCaves(std::vector<uint64_t>* cave_pages) {
	for (auto page = cave_pages->rbegin(); page != cave_pages->rend(); ++page) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(*page, kPageSize));
	}
	cave_pages->clear();
}

bool AllocateCave(Plan* plan, const Program& program, std::string* error) {
	std::vector<uint64_t> pages;
	for (const auto& write: plan->writes) {
		if (IsInsideProgram(program, write.target_address, write.off.size())) {
			continue;
		}
		if (!IsZero(write.off)) {
			return Fail(error, "non-zero write is outside the matched module");
		}

		const auto write_end = write.target_address + write.off.size();
		for (auto page = write.target_address & ~(kPageSize - 1); page < write_end;
		     page += kPageSize) {
			if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
				pages.push_back(page);
			}
		}
	}

	for (const auto page: pages) {
		const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    page, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "game_cheat_code_cave",
		    true);
		if (allocated != page) {
			if (allocated != 0) {
				Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
			}
			ReleaseCaves(&plan->cave_pages);
			return Fail(error, "could not allocate code-cave page");
		}
		plan->cave_pages.push_back(page);
	}
	return true;
}

bool ApplyWrites(Plan* plan, const Program& program, std::string* error) {
	SuppressAspectWrites(plan, true);
	if (plan->writes.empty()) {
		return true;
	}
	if (!AllocateCave(plan, program, error)) {
		return false;
	}

	for (int external = 1; external >= 0; external--) {
		for (const auto& write: plan->writes) {
			if (static_cast<int>(
			        !IsInsideProgram(program, write.target_address, write.on.size())) != external) {
				continue;
			}
			std::memcpy(reinterpret_cast<void*>(write.target_address), write.on.data(),
			            write.on.size());
			if (!Common::VirtualMemory::FlushInstructionCache(write.target_address,
			                                                  write.on.size())) {
				return Fail(error, "could not finalize cheat write");
			}
		}
	}
	return true;
}

} // namespace

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs) {
	Plan        plan;
	std::string error;
	if (!LoadPlan(plan_path, &plan, &error) || !ValidateGame(plan, main_program, &error)) {
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}
	SuppressAspectWrites(&plan, false);
	if (plan.writes.empty()) {
		return true;
	}

	g_pending_plan = std::make_unique<Plan>(std::move(plan));
	for (auto* program: programs) {
		if (!ApplyPending(program)) {
			return false;
		}
		if (g_pending_plan == nullptr) {
			return true;
		}
	}
	::printf("Game cheat: waiting for the matching module\n");
	return true;
}

bool ApplyPending(Program* program) {
	uint64_t source_base = 0;
	if (g_pending_plan == nullptr || program == nullptr || program->elf == nullptr ||
	    !Matches(g_pending_plan.get(), *program, &source_base)) {
		ApplyPendingAspect(program);
		return true;
	}
	::printf("Game cheat: matched %s with source base 0x%llx\n",
	         program->file_name.filename().string().c_str(),
	         static_cast<unsigned long long>(source_base));

	std::string error;
	if (!ApplyWrites(g_pending_plan.get(), *program, &error)) {
		ReleaseCaves(&g_pending_plan->cave_pages);
		g_pending_plan.reset();
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}

	for (const auto& name: g_pending_plan->mod_names) {
		::printf("Successfully applied cheat: %s\n", name.c_str());
	}
	g_applied_cave_pages = std::move(g_pending_plan->cave_pages);
	g_pending_plan.reset();
	ApplyPendingAspect(program);
	return true;
}

void InitializeWindowAspect(Program* main_program, const std::vector<Program*>& programs) {
	if (!Config::WindowAspectEnabled()) {
		return;
	}
	std::string title_id;
	std::string version;
	if (main_program == nullptr ||
	    !Common::EqualNoCase(main_program->file_name.filename().string(), "eboot.bin") ||
	    !SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version)) {
		::printf("Window aspect: skipped (game identity unavailable)\n");
		return;
	}
	const auto* profile = FindAspectProfile(title_id, version);
	if (profile == nullptr) {
		::printf("Window aspect: unsupported title/version %s %s\n", title_id.c_str(),
		         version.c_str());
		return;
	}
	{
		std::lock_guard lock(g_aspect_mutex);
		g_aspect_profile = profile;
		g_aspect_pending = true;
		if (g_window_width == 0 || g_window_height == 0) {
			g_window_width = Config::GetScreenWidth();
			g_window_height = Config::GetScreenHeight();
		}
	}
	for (auto* program: programs) {
		ApplyPendingAspect(program);
	}
	std::lock_guard lock(g_aspect_mutex);
	if (g_aspect_pending) {
		::printf("Window aspect: waiting for a unique matching executable\n");
	}
}

void UpdateWindowAspect(uint32_t width, uint32_t height) {
	if (!WindowAspectRatio(width, height)) {
		return;
	}
	std::lock_guard lock(g_aspect_mutex);
	g_window_width = width;
	g_window_height = height;
	if (g_aspect_page != 0) {
		StoreWindowAspect(*reinterpret_cast<uint32_t*>(g_aspect_page), width, height);
	}
}

void ReleaseWindowAspect(Program* program) {
	std::lock_guard lock(g_aspect_mutex);
	if (g_aspect_program == program && g_aspect_page != 0) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(g_aspect_page, kPageSize));
		g_aspect_page = 0;
		g_aspect_program = nullptr;
		g_aspect_instruction_address = 0;
		g_aspect_padding_address = 0;
	}
}

void Clear() {
	{
		std::lock_guard lock(g_aspect_mutex);
		g_aspect_pending = false;
		g_aspect_profile = nullptr;
		if (g_aspect_page != 0) {
			EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(g_aspect_page, kPageSize));
		}
		g_aspect_page = 0;
		g_aspect_program = nullptr;
		g_aspect_instruction_address = 0;
		g_aspect_padding_address = 0;
	}
	if (g_pending_plan != nullptr) {
		ReleaseCaves(&g_pending_plan->cave_pages);
	}
	g_pending_plan.reset();
	ReleaseCaves(&g_applied_cave_pages);
}

} // namespace Loader::GamePatch
