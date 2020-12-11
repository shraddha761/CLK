//
//  MemoryMap.hpp
//  Clock Signal
//
//  Created by Thomas Harte on 25/10/2020.
//  Copyright © 2020 Thomas Harte. All rights reserved.
//

#ifndef Machines_Apple_AppleIIgs_MemoryMap_hpp
#define Machines_Apple_AppleIIgs_MemoryMap_hpp

#include <array>
#include <vector>

#include "../AppleII/LanguageCardSwitches.hpp"
#include "../AppleII/AuxiliaryMemorySwitches.hpp"

namespace Apple {
namespace IIgs {

class MemoryMap {
	public:
		// MARK: - Initial construction and configuration.

		MemoryMap() : auxiliary_switches_(*this), language_card_(*this) {}

		void set_storage(std::vector<uint8_t> &ram, std::vector<uint8_t> &rom) {
			// Keep a pointer for later; also note the proper RAM offset.
			ram_base = ram.data();
			shadow_base[1] = &ram[ram.size() - 0x02'0000];	// i.e. all shadowed writes go somewhere in the last
															// 128bk of RAM.

			// Establish bank mapping.
			uint8_t next_region = 0;
			auto region = [&next_region, this]() -> uint8_t {
				assert(next_region != regions.size());
				return next_region++;
			};
			auto set_region = [this](uint8_t bank, uint16_t start, uint16_t end, uint8_t region) {
				assert((end == 0xffff) || !(end&0xff));
				assert(!(start&0xff));

				// Fill in memory map.
				size_t target = size_t((bank << 8) | (start >> 8));
				for(int c = start; c < end; c += 0x100) {
					region_map[target] = region;
					++target;
				}
			};
			auto set_regions = [this, set_region, region](uint8_t bank, std::initializer_list<uint16_t> addresses, std::vector<uint8_t> allocated_regions = {}) {
				uint16_t previous = 0x0000;
				auto next_region = allocated_regions.begin();
				for(uint16_t address: addresses) {
					set_region(bank, previous, address, next_region != allocated_regions.end() ? *next_region : region());
					previous = address;
					assert(next_region != allocated_regions.end() || allocated_regions.empty());
					if(next_region != allocated_regions.end()) ++next_region;
				}
				assert(next_region == allocated_regions.end());
			};

			// Current beliefs about the IIgs memory map:
			//
			//	* language card banking applies to banks $00, $01, $e0 and $e1;
			//	* auxiliary memory switches apply to bank $e0 only, but thereby also affect shadowed writes from $00;
			//	* shadowing may be enabled only on banks $00 and $01, or on all RAM pages; and
			//	* whether bit 16 of the address is passed to the Mega II is selectable — this affects both the destination
			//	of odd-bank shadows, and whether bank $e1 is actually distinct from $e0.
			//
			// So:
			//
			//	* banks $00 and $01 need to be divided both by shadowing zones and by the language card;
			//	* all other fast RAM banks need be divided by shadowing zone only;
			//	* $e0 needs to be ready for any language/auxiliary arrangement;
			//	* $e1 needs to apply the language card mapping only; and
			//	* ROM banks don't need to be divided? Or probably they shadow writes back to $e0/$e1 too?

			// Shadowing zones:
			//
			//	$0400–$0800	Text Page 1
			//	$0800–$0C00	Text Page 2								[ROM 03 machines]
			//	$2000–$4000	High-res Page 1, and Super High-res in odd banks
			//	$4000–$6000	High-res Page 2, and Huper High-res in odd banks
			//	$6000–$a000 Odd banks only, rest of Super High-res
			//	[plus IO and language card space, subject to your definition of shadowing]

			// Language card zones:
			//
			//	$D000–$E000	4kb window, into either bank 1 or bank 2
			//	$E000–end	12kb window, always the same RAM.

			// Auxiliary zones:
			//
			//	$0000–$0200	Zero page (and stack)
			//	$0200–$0400	[space in between]
			//	$0400–$0800	Text Page 1
			//	$0800–$2000	[space in between]
			//	$2000–$4000	High-res Page 1
			//	$4000–$C000	[space in between]

			// Card zones:
			//
			//	$C100–$C2FF	either cards or IIe-style ROM
			//	$C300–$C3FF	IIe-supplied 80-column card replacement ROM
			//	$C400–$C7FF either cards or IIe-style ROM
			//	$C800–$CFFF	Standard extended card area

			// Reserve region 0 as that for unmapped memory.
			region();

			// Bank $00: all locations potentially affected by the auxiliary switches or the
			// language switches. Which will naturally align with shadowable zones.
			set_regions(0x00, {
				0x0200,	0x0400,	0x0800, 0x0c00,
				0x2000,	0x4000, 0x6000,
				0xc000,	0xc100,	0xc300,	0xc400,	0xc800,
				0xd000,	0xe000,
				0xffff
			});

			// Bank $01: all locations potentially affected by the language switches, by shadowing,
			// or marked for IO.
			set_regions(0x01, {
				0x0400,	0x0800, 0x0c00,
				0x2000,	0x4000, 0x6000, 0xa000,
				0xc000,	0xc100,	0xc300,	0xc400,	0xc800,
				0xd000,	0xe000,
				0xffff
			});

			// Banks $02–[end of RAM]: all locations potentially affected by shadowing.
			const uint8_t fast_ram_bank_count = uint8_t((ram.size() - 0x02'0000) / 0x01'0000);
			if(fast_ram_bank_count > 2) {
				const std::vector<uint8_t> evens = {
					region(),	// 0x0000 – 0x0400.
					region(),	// 0x0400 – 0x0800.
					region(),	// 0x0800 – 0x0c00.
					region(),	// 0x0c00 – 0x2000.
					region(),	// 0x2000 – 0x4000.
					region(),	// 0x4000 – 0x6000.
					region(),	// 0x6000 – [end].
				};
				const std::vector<uint8_t> odds = {
					region(),	// 0x0000 – 0x0400.
					region(),	// 0x0400 – 0x0800.
					region(),	// 0x0800 – 0x0c00.
					region(),	// 0x0c00 – 0x2000.
					region(),	// 0x2000 – 0x4000.
					region(),	// 0x4000 – 0x6000.
					region(),	// 0x6000 – 0xa000.
					region(),	// 0xa000 – [end].
				};
				for(uint8_t bank = 0x02; bank < fast_ram_bank_count; bank += 2) {
					set_regions(bank,	{0x0400, 0x0800, 0x0c00, 0x2000, 0x4000, 0x6000, 0xffff}, evens);
					set_regions(bank+1,	{0x0400, 0x0800, 0x0c00, 0x2000, 0x4000, 0x6000, 0xa000, 0xffff}, odds);
				}
			}

			// [Banks $80–$e0: empty].

			// Banks $e0, $e1: all locations potentially affected by the language switches or marked for IO.
			// Alas, separate regions are needed due to the same ROM appearing on both pages.
			for(uint8_t c = 0; c < 2; c++) {
				set_regions(0xe0 + c, {0xc000, 0xc100, 0xc300, 0xc400, 0xc800, 0xd000, 0xe000, 0xffff});
			}

			// [Banks $e2–[ROM start]: empty].

			// ROM banks: directly mapped to ROM.
			const uint8_t rom_bank_count = uint8_t(rom.size() >> 16);
			const uint8_t first_rom_bank = uint8_t(0x100 - rom_bank_count);
			const uint8_t rom_region = region();
			for(uint8_t c = 0; c < rom_bank_count; ++c) {
				set_region(first_rom_bank + c, 0x0000, 0xffff, rom_region);
			}

			// Apply proper storage to those banks.
			auto set_storage = [this](uint32_t address, const uint8_t *read, uint8_t *write) {
				// Don't allow the reserved null region to be modified.
				assert(region_map[address >> 8]);

				// Either set or apply a quick bit of testing as to the logic at play.
				auto &region = regions[region_map[address >> 8]];
				if(read) read -= address;
				if(write) write -= address;
				if(!region.read) {
					region.read = read;
					region.write = write;
				} else {
					assert(region.read == read);
					assert(region.write == write);
				}
			};

			// This is highly redundant, but decouples this step from the above.
			for(size_t c = 0; c < 0x80'0000; c += 0x100) {
				if(c < ram.size() - 0x02'0000) {
					set_storage(uint32_t(c), &ram[c], &ram[c]);
				}
			}
			uint8_t *const slow_ram = &ram[ram.size() - 0x02'0000] - 0xe0'0000;
			for(size_t c = 0xe0'0000; c < 0xe2'0000; c += 0x100) {
				set_storage(uint32_t(c), &slow_ram[c], &slow_ram[c]);
			}
			for(uint32_t c = 0; c < uint32_t(rom_bank_count); c++) {
				set_storage((first_rom_bank + c) << 16, &rom[c << 16], nullptr);
			}

			// TODO: set 1Mhz flags.

			// Apply initial language/auxiliary state.
			set_all_paging();
		}

		// MARK: - Live bus access notifications and register access.

		void set_shadow_register(uint8_t value) {
			const uint8_t diff = value ^ shadow_register_;
			shadow_register_ = value;

			if(diff & 0x40) {	// IO/language-card inhibit.
				set_language_card_paging();
				set_card_paging();
			}

			if(diff & 0x3f) {
				set_shadowing();
			}
		}

		uint8_t get_shadow_register() const {
			return shadow_register_;
		}

		void set_speed_register(uint8_t value) {
			const uint8_t diff = value ^ speed_register_;
			speed_register_ = value;
			if(diff & 0x10) {
				set_shadowing();
			}
		}

		void set_state_register(uint8_t value) {
			auxiliary_switches_.set_state(value);
			language_card_.set_state(value);
		}

		uint8_t get_state_register() const {
			return language_card_.get_state() | auxiliary_switches_.get_state();
		}

		void access(uint16_t address, bool is_read) {
			auxiliary_switches_.access(address, is_read);
			if((address & 0xfff0) == 0xc080) language_card_.access(address, is_read);
		}

		using AuxiliaryMemorySwitches = Apple::II::AuxiliaryMemorySwitches<MemoryMap>;
		const AuxiliaryMemorySwitches &auxiliary_switches() const {
			return auxiliary_switches_;
		}

		using LanguageCardSwitches = Apple::II::LanguageCardSwitches<MemoryMap>;
		const LanguageCardSwitches &language_card_switches() const {
			return language_card_;
		}

	private:
		AuxiliaryMemorySwitches auxiliary_switches_;
		LanguageCardSwitches language_card_;
		friend AuxiliaryMemorySwitches;
		friend LanguageCardSwitches;

		uint8_t shadow_register_ = 0x08;
		uint8_t speed_register_ = 0x00;

		// MARK: - Memory banking.

		// Cf. LanguageCardSwitches; this function should update the region from
		// $D000 onwards as per the state of the language card flags — there may
		// end up being ROM or RAM (or auxiliary RAM), and the first 4kb of it
		// may be drawn from either of two pools.
		void set_language_card_paging() {
			const auto language_state = language_card_.state();
			const auto zero_state = auxiliary_switches_.zero_state();
			const bool inhibit_banks0001 = shadow_register_ & 0x40;

			auto apply = [&language_state, this](uint32_t bank_base, uint8_t *ram) {
				// This assumes bank 1 is the one before bank 2 when RAM is linear.
				uint8_t *const d0_ram_bank = ram - (language_state.bank2 ? 0x0000 : 0x1000);

				// Crib the ROM pointer from a page it's always visible on.
				const uint8_t *const rom = &regions[region_map[0xffd0]].read[0xff'd000] - ((bank_base << 8) + 0xd000);

				auto &d0_region = regions[region_map[bank_base | 0xd0]];
				d0_region.read = language_state.read ? d0_ram_bank : rom;
				d0_region.write = language_state.write ? nullptr : d0_ram_bank;

				auto &e0_region = regions[region_map[bank_base | 0xe0]];
				e0_region.read = language_state.read ? ram : rom;
				e0_region.write = language_state.write ? nullptr : ram;

				// Assert assumptions made above re: memory layout.
				assert(region_map[bank_base | 0xd0] + 1 == region_map[bank_base | 0xe0]);
				assert(region_map[bank_base | 0xe0] == region_map[bank_base | 0xff]);
			};
			auto set_no_card = [this](uint32_t bank_base) {
				auto &d0_region = regions[region_map[bank_base | 0xd0]];
				d0_region.read = ram_base;
				d0_region.write = ram_base;

				auto &e0_region = regions[region_map[bank_base | 0xe0]];
				e0_region.read = ram_base;
				e0_region.write = ram_base;

				// Assert assumptions made above re: memory layout.
				assert(region_map[bank_base | 0xd0] + 1 == region_map[bank_base | 0xe0]);
				assert(region_map[bank_base | 0xe0] == region_map[bank_base | 0xff]);
			};

			if(inhibit_banks0001) {
				set_no_card(0x0000);
				set_no_card(0x0100);
			} else {
				apply(0x0000, zero_state ? &ram_base[0x01'0000] : ram_base);
				apply(0x0100, ram_base);
			}

			// The pointer stored in region_map[0xe000] has already been adjusted for
			// the 0xe0'0000 addressing offset.
			uint8_t *const e0_ram = regions[region_map[0xe000]].write;
			apply(0xe000, e0_ram);
			apply(0xe100, e0_ram);
		}

		// Cf. AuxiliarySwitches; this should establish whether ROM or card switches
		// are exposed in the distinct regions C100–C2FF, C300–C3FF, C400–C7FF and
		// C800–CFFF.
		//
		// On the IIgs it intersects with the current shadow register.
		//
		// TODO: so... shouldn't the card mask be incorporated here? I've got it implemented
		// distinctly at present, but does that create any invalid state interactions?
		void set_card_paging() {
			const bool inhibit_banks0001 = shadow_register_ & 0x40;
			const auto state = auxiliary_switches_.card_state();

			auto apply = [&state, this](uint32_t bank_base) {
				auto &c0_region = regions[region_map[bank_base | 0xc0]];
				auto &c1_region = regions[region_map[bank_base | 0xc1]];
				auto &c3_region = regions[region_map[bank_base | 0xc3]];
				auto &c4_region = regions[region_map[bank_base | 0xc4]];
				auto &c8_region = regions[region_map[bank_base | 0xc8]];

				const uint8_t *const rom = &regions[region_map[0xffd0]].read[0xffc100] - ((bank_base << 8) + 0xc100);

				// This is applied dynamically as it may be added or lost in banks $00 and $01.
				c0_region.flags |= Region::IsIO;

#define apply_region(flag, region)	\
				if(flag) {	\
					region.read = rom;	\
					region.flags &= ~Region::IsIO;	\
				} else {	\
					region.flags |= Region::IsIO;	\
				}

				apply_region(state.region_C1_C3, c1_region);
				apply_region(state.region_C3, c3_region);
				apply_region(state.region_C4_C8, c4_region);
				apply_region(state.region_C8_D0, c8_region);

#undef apply_region

				// Sanity checks.
				assert(region_map[bank_base | 0xc1] == region_map[bank_base | 0xc0]+1);
				assert(region_map[bank_base | 0xc3] == region_map[bank_base | 0xc1]+1);
				assert(region_map[bank_base | 0xc4] == region_map[bank_base | 0xc3]+1);
				assert(region_map[bank_base | 0xc8] == region_map[bank_base | 0xc4]+1);
				assert(region_map[bank_base | 0xd0] == region_map[bank_base | 0xc8]+1);
			};

			if(inhibit_banks0001) {
				// Set no IO in the Cx00 range for banks $00 and $01, just
				// regular RAM (or possibly auxiliary).
				const auto auxiliary_state = auxiliary_switches_.main_state();
				for(uint8_t region = region_map[0x00c0]; region < region_map[0x00d0]; region++) {
					regions[region].read = auxiliary_state.base.read ? &ram_base[0x01'0000] : ram_base;
					regions[region].write = auxiliary_state.base.write ? &ram_base[0x01'0000] : ram_base;
					regions[region].flags &= ~Region::IsIO;
				}
				for(uint8_t region = region_map[0x01c0]; region < region_map[0x01d0]; region++) {
					regions[region].read = regions[region].write = ram_base;
					regions[region].flags &= ~Region::IsIO;
				}
			} else {
				// Obey the card state for banks $00 and $01.
				apply(0x0000);
				apply(0x0100);
			}

			// Obey the card state for banks $e0 and $e1.
			apply(0xe000);
			apply(0xe100);
		}

		// Cf. LanguageCardSwitches; this should update whether base or auxiliary RAM is
		// visible in: (i) the zero and stack pages; and (ii) anywhere that the language
		// card is exposing RAM instead of ROM.
		void set_zero_page_paging() {
			// Affects bank $00 only, and should be a single region.
			auto &region = regions[region_map[0]];
			region.read = region.write = auxiliary_switches_.zero_state() ? &ram_base[0x01'0000] : ram_base;
			assert(region_map[0x0000]+1 == region_map[0x0002]);

			// Switching to or from auxiliary RAM potentially affects the
			// language card area.
			set_language_card_paging();
		}

		// IIgs specific: sets or resets the ::IsShadowed flag across affected banks as
		// per the current state of the shadow register.
		//
		// Completely distinct from the auxiliary and language card switches.
		void set_shadowing() {
			const bool inhibit_all_pages = !(speed_register_ & 0x10);

			// Disables shadowing for the region starting from @c zone if @c flag is true;
			// otherwise enables it.
#define apply(flag, zone)	\
	if(flag) {	\
		regions[region_map[zone]].flags &= ~Region::IsShadowed; \
	} else {	\
		regions[region_map[zone]].flags |= Region::IsShadowed; \
	}

			printf("Shadowing: %02x\n", shadow_register_);

			// Relevant bits:
			//
			//	b5: inhibit shadowing, text page 2	[if ROM 03; as if always set otherwise]
			//	b4: inhibit shadowing, auxiliary high-res graphics
			//	b3: inhibit shadowing, super high-res graphics
			//	b2: inhibit shadowing, high-res graphics page 2
			//	b1: inhibit shadowing, high-res graphics page 1
			//	b0: inhibit shadowing, text page 1
			//
			// The interpretations of how the overlapping high-res and super high-res inhibit
			// bits apply used below is taken from The Apple IIgs Technical Reference, P. 178.

			// Text Page 1, main and auxiliary — $0400–$0800.
			apply(shadow_register_ & 0x01, 0x0004);
			apply(shadow_register_ & 0x01, 0x0104);
			apply((shadow_register_ & 0x01) || inhibit_all_pages, 0x0204);	// All other pages uses the same shadowing flags.
			apply((shadow_register_ & 0x01) || inhibit_all_pages, 0x0304);
			assert(region_map[0x0008] == region_map[0x0004]+1);
			assert(region_map[0x0108] == region_map[0x0104]+1);
			assert(region_map[0x0208] == region_map[0x0204]+1);
			assert(region_map[0x0308] == region_map[0x0304]+1);

			// Text Page 2, main and auxiliary — 0x0800–0x0c00.
			// TODO: on a ROM03 machine only.
			apply(shadow_register_ & 0x20, 0x0008);
			apply(shadow_register_ & 0x20, 0x0108);
			apply((shadow_register_ & 0x20) || inhibit_all_pages, 0x0208);
			apply((shadow_register_ & 0x20) || inhibit_all_pages, 0x0308);
			assert(region_map[0x000c] == region_map[0x0008]+1);
			assert(region_map[0x010c] == region_map[0x0108]+1);
			assert(region_map[0x020c] == region_map[0x0208]+1);
			assert(region_map[0x030c] == region_map[0x0308]+1);

			// Hi-res graphics Page 1, main and auxiliary — $2000–$4000;
			// also part of the super high-res graphics page.
			//
			// Test applied: both the graphics page inhibit and, on odd
			// pages, the super high-res inhibit must be applied to inhibit
			// this area.
			apply(shadow_register_ & 0x02, 0x0020);
			apply((shadow_register_ & 0x12) && (shadow_register_ & 0x08), 0x0120);
			apply((shadow_register_ & 0x02) || inhibit_all_pages, 0x0220);
			apply(((shadow_register_ & 0x12) && (shadow_register_ & 0x08)) || inhibit_all_pages, 0x0320);
			assert(region_map[0x0040] == region_map[0x0020]+1);
			assert(region_map[0x0140] == region_map[0x0120]+1);
			assert(region_map[0x0240] == region_map[0x0220]+1);
			assert(region_map[0x0340] == region_map[0x0320]+1);

			// Hi-res graphics Page 2, main and auxiliary — $4000–$6000;
			// also part of the super high-res graphics page.
			//
			// Test applied: both the graphics page inhibit and, on odd
			// pages, the super high-res inhibit must be applied to inhibit
			// this area.
			apply(shadow_register_ & 0x04, 0x0040);
			apply((shadow_register_ & 0x14) && (shadow_register_ & 0x08), 0x0140);
			apply((shadow_register_ & 0x04) || inhibit_all_pages, 0x0240);
			apply(((shadow_register_ & 0x14) && (shadow_register_ & 0x08)) || inhibit_all_pages, 0x0340);
			assert(region_map[0x0060] == region_map[0x0040]+1);
			assert(region_map[0x0160] == region_map[0x0140]+1);
			assert(region_map[0x0260] == region_map[0x0240]+1);
			assert(region_map[0x0360] == region_map[0x0340]+1);

			// Residue of Super Hi-Res — $6000–$a000 (odd pages only).
			apply(shadow_register_ & 0x08, 0x0160);
			apply((shadow_register_ & 0x08) || inhibit_all_pages, 0x0360);
			assert(region_map[0x01a0] == region_map[0x0160]+1);
			assert(region_map[0x03a0] == region_map[0x0360]+1);

#undef apply
		}

		// Cf. the AuxiliarySwitches; establishes whether main or auxiliary RAM
		// is exposed in bank $00 for a bunch of regions.
		void set_main_paging() {
			const auto state = auxiliary_switches_.main_state();

#define set(page, flags)	{\
			auto &region = regions[region_map[page]];	\
			region.read = flags.read ? &ram_base[0x01'0000] : ram_base;	\
			region.write = flags.write ? &ram_base[0x01'0000] : ram_base;	\
		}

			// Base: $0200–$03FF.
			set(0x02, state.base);
			assert(region_map[0x02] == region_map[0x00]+1);
			assert(region_map[0x04] == region_map[0x02]+1);

			// Region $0400–$07ff.
			set(0x04, state.region_04_08);
			assert(region_map[0x08] == region_map[0x04]+1);

			// Base: $0800–$1FFF.
			set(0x08, state.base);
			set(0x0c, state.base);
			assert(region_map[0x0c] == region_map[0x08]+1);
			assert(region_map[0x20] == region_map[0x0c]+1);

			// Region $2000–$3FFF.
			set(0x20, state.region_20_40);
			assert(region_map[0x40] == region_map[0x20]+1);

			// Base: $4000–$BFFF.
			set(0x40, state.base);
			set(0x60, state.base);
			assert(region_map[0x60] == region_map[0x40]+1);
			assert(region_map[0xc0] == region_map[0x60]+1);

#undef set

			// This also affects shadowing flags, if shadowing is enabled at all,
			// and might affect RAM in the IO area of bank $00 because the language
			// card can be inhibited on a IIgs.
			set_card_paging();
		}

		void set_all_paging() {
			set_card_paging();
			set_zero_page_paging();	// ... which calls set_language_card_paging().
			set_main_paging();
			set_shadowing();
		}

		// Throwaway storage to facilitate branchless handling of shadowing.
		uint8_t shadow_throwaway_;

	public:
		// Memory layout here is done via double indirection; the main loop should:
		//	(i) use the top two bytes of the address to get an index from memory_map_; and
		//	(ii) use that to index the memory_regions table.
		//
		// Pointers are eight bytes at the time of writing, so the extra level of indirection
		// reduces what would otherwise be a 1.25mb table down to not a great deal more than 64kb.
		std::array<uint8_t, 65536> region_map{};
		uint8_t *ram_base = nullptr;
		uint8_t *shadow_base[2] = {&shadow_throwaway_, nullptr};
		static constexpr int shadow_mask[2] = {0, 0x01'ffff};

		struct Region {
			uint8_t *write = nullptr;
			const uint8_t *read = nullptr;
			uint8_t flags = 0;

			enum Flag: uint8_t {
				IsShadowed = 1 << 0,	// Writes should be shadowed to [end of RAM - 128kb + base offset].
				Is1Mhz = 1 << 1,		// Both reads and writes should be synchronised with the 1Mhz clock.
				IsIO = 1 << 2,			// Indicates that this region should be checked for soft switches, registers, etc;
										// usurps the shadowed flags.
			};
		};
		std::array<Region, 64> regions;	// The assert above ensures that this is large enough; there's no
										// doctrinal reason for it to be whatever size it is now, just
										// adjust as required.
};

// TODO: branching below on region.read/write is predicated on the idea that extra scratch space
// would be less efficient. Verify that?

#define MemoryMapRegion(map, address) map.regions[map.region_map[address >> 8]]
#define MemoryMapRead(region, address, value) *value = region.read ? region.read[address] : 0xff
#define MemoryMapWrite(map, region, address, value) \
	if(region.write) {	\
		region.write[address] = *value;	\
		const bool is_shadowed = region.flags & MemoryMap::Region::IsShadowed;	\
		map.shadow_base[is_shadowed][(&region.write[address] - map.ram_base) & map.shadow_mask[is_shadowed]] = *value;	\
	}

// Quick notes on ::IsShadowed contortions:
//
// The objective is to support shadowing:
//	1. without storing a whole extra pointer, and such that the shadowing flags are orthogonal to the current auxiliary memory settings;
//	2. in such a way as to support shadowing both in banks $00/$01 and elsewhere; and
//	3. to do so without introducing too much in the way of branching.
//
// Hence the implemented solution: if shadowing is enabled then use the distance from the start of physical RAM
// modulo 128k indexed into the bank $e0/$e1 RAM.
//
// With a further twist: the modulo and pointer are indexed on ::IsShadowed to eliminate a branch even on that.

}
}

#endif /* MemoryMap_h */
