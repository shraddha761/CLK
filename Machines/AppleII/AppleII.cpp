//
//  AppleII.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 14/04/2018.
//  Copyright 2018 Thomas Harte. All rights reserved.
//

#include "AppleII.hpp"

#include "../../Activity/Source.hpp"
#include "../MediaTarget.hpp"
#include "../CRTMachine.hpp"
#include "../JoystickMachine.hpp"
#include "../KeyboardMachine.hpp"
#include "../Utility/MemoryFuzzer.hpp"
#include "../Utility/StringSerialiser.hpp"

#include "../../Processors/6502/6502.hpp"
#include "../../Components/AudioToggle/AudioToggle.hpp"

#include "../../Outputs/Speaker/Implementation/LowpassSpeaker.hpp"

#include "Card.hpp"
#include "DiskIICard.hpp"
#include "Video.hpp"

#include "../../Analyser/Static/AppleII/Target.hpp"
#include "../../ClockReceiver/ForceInline.hpp"
#include "../../Configurable/Configurable.hpp"
#include "../../Storage/Disk/Track/TrackSerialiser.hpp"
#include "../../Storage/Disk/Encodings/AppleGCR/SegmentParser.hpp"

#include <algorithm>
#include <array>
#include <memory>

std::vector<std::unique_ptr<Configurable::Option>> AppleII::get_options() {
	std::vector<std::unique_ptr<Configurable::Option>> options;
	options.emplace_back(new Configurable::BooleanOption("Accelerate DOS 3.3", "quickload"));
	return options;
}

namespace {

template <bool is_iie> class ConcreteMachine:
	public CRTMachine::Machine,
	public MediaTarget::Machine,
	public KeyboardMachine::Machine,
	public Configurable::Device,
	public CPU::MOS6502::BusHandler,
	public Inputs::Keyboard,
	public AppleII::Machine,
	public Activity::Source,
	public JoystickMachine::Machine,
	public AppleII::Card::Delegate {
	private:
		struct VideoBusHandler : public AppleII::Video::BusHandler {
			public:
				VideoBusHandler(uint8_t *ram, uint8_t *aux_ram) : ram_(ram), aux_ram_(aux_ram) {}

				uint8_t perform_read(uint16_t address) {
					return ram_[address];
				}
				uint16_t perform_aux_read(uint16_t address) {
					return static_cast<uint16_t>(ram_[address] | (aux_ram_[address] << 8));
				}

			private:
				uint8_t *ram_, *aux_ram_;
		};

		CPU::MOS6502::Processor<ConcreteMachine, false> m6502_;
		VideoBusHandler video_bus_handler_;
		std::unique_ptr<AppleII::Video::Video<VideoBusHandler>> video_;
		int cycles_into_current_line_ = 0;
		Cycles cycles_since_video_update_;

		void update_video() {
			video_->run_for(cycles_since_video_update_.flush());
		}
		static const int audio_divider = 8;
		void update_audio() {
			speaker_.run_for(audio_queue_, cycles_since_audio_update_.divide(Cycles(audio_divider)));
		}
		void update_just_in_time_cards() {
			for(const auto &card : just_in_time_cards_) {
				card->run_for(cycles_since_card_update_, stretched_cycles_since_card_update_);
			}
			cycles_since_card_update_ = 0;
			stretched_cycles_since_card_update_ = 0;
		}

		uint8_t ram_[65536], aux_ram_[65536];
		std::vector<uint8_t> rom_;
		std::vector<uint8_t> character_rom_;
		uint8_t keyboard_input_ = 0x00;

		Concurrency::DeferringAsyncTaskQueue audio_queue_;
		Audio::Toggle audio_toggle_;
		Outputs::Speaker::LowpassSpeaker<Audio::Toggle> speaker_;
		Cycles cycles_since_audio_update_;

		// MARK: - Cards
		std::array<std::unique_ptr<AppleII::Card>, 7> cards_;
		Cycles cycles_since_card_update_;
		std::vector<AppleII::Card *> every_cycle_cards_;
		std::vector<AppleII::Card *> just_in_time_cards_;

		int stretched_cycles_since_card_update_ = 0;

		void install_card(std::size_t slot, AppleII::Card *card) {
			assert(slot >= 1 && slot < 8);
			cards_[slot - 1].reset(card);
			card->set_delegate(this);
			pick_card_messaging_group(card);
		}

		bool is_every_cycle_card(AppleII::Card *card) {
			return !card->get_select_constraints();
		}

		void pick_card_messaging_group(AppleII::Card *card) {
			const bool is_every_cycle = is_every_cycle_card(card);
			std::vector<AppleII::Card *> &intended = is_every_cycle ? every_cycle_cards_ : just_in_time_cards_;
		 	std::vector<AppleII::Card *> &undesired = is_every_cycle ? just_in_time_cards_ : every_cycle_cards_;

			if(std::find(intended.begin(), intended.end(), card) != intended.end()) return;
			auto old_membership = std::find(undesired.begin(), undesired.end(), card);
			if(old_membership != undesired.end()) undesired.erase(old_membership);
			intended.push_back(card);
		}

		void card_did_change_select_constraints(AppleII::Card *card) override {
			pick_card_messaging_group(card);
		}

		AppleII::DiskIICard *diskii_card() {
			return dynamic_cast<AppleII::DiskIICard *>(cards_[5].get());
		}

		// MARK: - Memory Map.

		/*
			The Apple II's paging mechanisms are byzantine to say the least. Painful is
			another appropriate adjective.

			On a II and II+ there are five distinct zones of memory:

			0000 to c000	:	the main block of RAM
			c000 to d000	:	the IO area, including card ROMs
			d000 to e000	:	the low ROM area, which can alternatively contain either one of two 4kb blocks of RAM with a language card
			e000 onward		:	the rest of ROM, also potentially replaced with RAM by a language card

			On a IIe with auxiliary memory the following orthogonal changes also need to be factored in:

			0000 to 0200 	:	can be paged independently of the rest of RAM, other than part of the language card area which pages with it
			0400 to 0800	:	the text screen, can be configured to write to auxiliary RAM
			2000 to 4000	:	the graphics screen, which can be configured to write to auxiliary RAM
			c100 to d000	:	can be used to page an additional 3.75kb of ROM, replacing the IO area
			c300 to c400	:	can contain the same 256-byte segment of the ROM as if the whole IO area were switched, but while leaving cards visible in the rest

			If dealt with as individual blocks in the inner loop, that would therefore imply mapping
			an address to one of 12 potential pageable zones. So I've gone reductive and surrendered
			to paging every 6502 page of memory independently. It makes the paging events more expensive,
			but hopefully is clear.

			Those 12 block, for the record:

			0000 to 0200;		0200 to 0400;		0400 to 0800;		0800 to 2000;
			2000 to 4000;		4000 to c000;		c000 to c100;		c100 to c300;
			c300 to c400;		c400 to d000;		d000 to e000;		e000+
		*/
		uint8_t *read_pages_[256];	// each is a pointer to the 256-block of memory the CPU should read when accessing that page of memory
		uint8_t *write_pages_[256];	// as per read_pages_, but this is where the CPU should write. If a pointer is nullptr, don't write.

		// MARK: - The language card.
		struct {
			bool bank1 = false;
			bool read = false;
			bool pre_write = false;
			bool write = false;
		} language_card_;
		bool has_language_card_ = true;
		void set_language_card_paging() {
			uint8_t *const ram = alternative_zero_page_ ? aux_ram_ : ram_;
			uint8_t *const rom = is_iie ? &rom_[3840] : rom_.data();

			for(int target = 0xd0; target < 0x100; ++target) {
				uint8_t *const ram_page = &ram[(target << 8) - ((target < 0xe0 && language_card_.bank1) ? 0x1000 : 0x0000)];
				uint8_t *const rom_page = &rom[(target << 8) - 0xd000];
				write_pages_[target] = has_language_card_ && !language_card_.write ? ram_page : nullptr;
				read_pages_[target] = has_language_card_ && language_card_.read ? ram_page : rom_page;
			}
		}

		// MARK - The IIe's ROM controls.
		bool internal_CX_rom_ = false;
		bool slot_C3_rom_ = false;
//		bool internal_c8_rom_ = false;

		void set_card_paging() {
			for(int c = 0xc1; c < 0xd0; ++c) {
				read_pages_[c] = internal_CX_rom_ ? &rom_[static_cast<size_t>(c << 8) - 0xc100] : nullptr;
			}
			if(slot_C3_rom_) read_pages_[0xc3] = &rom_[0xc300 - 0xc100];
		}


		// MARK - The IIe's auxiliary RAM controls.
		bool alternative_zero_page_ = false;
		bool read_auxiliary_memory_ = false;
		bool write_auxiliary_memory_ = false;
		void set_main_paging() {
			printf("80store: %d; page2:%d; rdaux: %d; wraux:%d\n", video_->get_80_store(), video_->get_page2(), read_auxiliary_memory_, write_auxiliary_memory_);
			bool store_80 = video_->get_80_store();
			for(int target = 0x02; target < 0xc0; ++target) {
				write_pages_[target] = !store_80 && write_auxiliary_memory_ ? &aux_ram_[target << 8] : &ram_[target << 8];
				read_pages_[target] = !store_80 && read_auxiliary_memory_ ? &aux_ram_[target << 8] : &ram_[target << 8];
			}

			if(store_80) {
				int start_page, end_page;
				if(video_->get_text()) {
					start_page = 0x4;
					end_page = 0x8;
				} else {
					start_page = 0x10;
					end_page = 0x20;
				}

				bool use_aux_ram = video_->get_page2();
				for(int target = start_page; target < end_page; ++target) {
					write_pages_[target] = use_aux_ram ? &aux_ram_[target << 8] : &ram_[target << 8];
					read_pages_[target] = use_aux_ram ? &aux_ram_[target << 8] : &ram_[target << 8];
				}
			}
		}

		// MARK - typing
		std::unique_ptr<Utility::StringSerialiser> string_serialiser_;

		// MARK - quick loading
		bool should_load_quickly_ = false;

		// MARK - joysticks
		class Joystick: public Inputs::ConcreteJoystick {
			public:
				Joystick() :
					ConcreteJoystick({
						Input(Input::Horizontal),
						Input(Input::Vertical),

						// The Apple II offers three buttons between two joysticks;
						// this emulator puts three buttons on each joystick and
						// combines them.
						Input(Input::Fire, 0),
						Input(Input::Fire, 1),
						Input(Input::Fire, 2),
					}) {}

					void did_set_input(const Input &input, float value) override {
						if(!input.info.control.index && (input.type == Input::Type::Horizontal || input.type == Input::Type::Vertical))
							axes[(input.type == Input::Type::Horizontal) ? 0 : 1] = 1.0f - value;
					}

					void did_set_input(const Input &input, bool value) override {
						if(input.type == Input::Type::Fire && input.info.control.index < 3) {
							buttons[input.info.control.index] = value;
						}
					}

				bool buttons[3] = {false, false, false};
				float axes[2] = {0.5f, 0.5f};
		};

		// On an Apple II, the programmer strobes 0xc070 and that causes each analogue input
		// to begin a charge and discharge cycle **if they are not already charging**.
		// The greater the analogue input, the faster they will charge and therefore the sooner
		// they will discharge.
		//
		// This emulator models that with analogue_charge_ being essentially the amount of time,
		// in charge threshold units, since 0xc070 was last strobed. But if any of the analogue
		// inputs were already partially charged then they gain a bias in analogue_biases_.
		//
		// It's a little indirect, but it means only having to increment the one value in the
		// main loop.
		float analogue_charge_ = 0.0f;
		float analogue_biases_[4] = {0.0f, 0.0f, 0.0f, 0.0f};

		std::vector<std::unique_ptr<Inputs::Joystick>> joysticks_;
		bool analogue_channel_is_discharged(size_t channel) {
			return static_cast<Joystick *>(joysticks_[channel >> 1].get())->axes[channel & 1] < analogue_charge_ + analogue_biases_[channel];
		}

	public:
		ConcreteMachine(const Analyser::Static::AppleII::Target &target, const ROMMachine::ROMFetcher &rom_fetcher):
		 	m6502_(*this),
		 	video_bus_handler_(ram_, aux_ram_),
		 	audio_toggle_(audio_queue_),
		 	speaker_(audio_toggle_) {
		 	// The system's master clock rate.
		 	const float master_clock = 14318180.0;

		 	// This is where things get slightly convoluted: establish the machine as having a clock rate
		 	// equal to the number of cycles of work the 6502 will actually achieve. Which is less than
		 	// the master clock rate divided by 14 because every 65th cycle is extended by one seventh.
			set_clock_rate((master_clock / 14.0) * 65.0 / (65.0 + 1.0 / 7.0));

			// The speaker, however, should think it is clocked at half the master clock, per a general
			// decision to sample it at seven times the CPU clock (plus stretches).
			speaker_.set_input_rate(static_cast<float>(master_clock / (2.0 * static_cast<float>(audio_divider))));

			// Apply a 6Khz low-pass filter. This was picked by ear and by an attempt to understand the
			// Apple II schematic but, well, I don't claim much insight on the latter. This is definitely
			// something to review in the future.
			speaker_.set_high_frequency_cutoff(6000);

			// Also, start with randomised memory contents.
			Memory::Fuzz(ram_, sizeof(ram_));
			Memory::Fuzz(aux_ram_, sizeof(aux_ram_));

			// Add a couple of joysticks.
		 	joysticks_.emplace_back(new Joystick);
		 	joysticks_.emplace_back(new Joystick);

			// Pick the required ROMs.
			using Target = Analyser::Static::AppleII::Target;
			std::vector<std::string> rom_names = {"apple2-character.rom"};
			size_t rom_size = 12*1024;
			switch(target.model) {
				default:
					rom_names.push_back("apple2o.rom");
				break;
				case Target::Model::IIplus:
					rom_names.push_back("apple2.rom");
				break;
				case Target::Model::IIe:
					rom_size += 3840;
					rom_names.push_back("apple2eu.rom");
				break;
			}
			const auto roms = rom_fetcher("AppleII", rom_names);

			if(!roms[0] || !roms[1]) {
				throw ROMMachine::Error::MissingROMs;
			}

			rom_ = std::move(*roms[1]);
			if(rom_.size() > rom_size) {
				rom_.erase(rom_.begin(), rom_.end() - static_cast<off_t>(rom_size));
			}

			character_rom_ = std::move(*roms[0]);

			if(target.disk_controller != Target::DiskController::None) {
				// Apple recommended slot 6 for the (first) Disk II.
				install_card(6, new AppleII::DiskIICard(rom_fetcher, target.disk_controller == Target::DiskController::SixteenSector));
			}

			// Set up the block that will provide CX ROM access on a IIe.
//			cx_rom_block_.read_pointer = rom_.data();

			// Set up the default memory blocks. On a II or II+ these values will never change.
			// On a IIe they'll be affected by selection of auxiliary RAM.
			for(int c = 0; c < 0xc0; ++c) {
				read_pages_[c] = write_pages_[c] = &ram_[c << 8];
			}

			// Set the whole card area to initially backed by nothing.
			for(int c = 0xc0; c < 0xd0; ++c) {
				read_pages_[c] = write_pages_[c] = nullptr;
			}

			// Set proper values for the language card/ROM area.
			set_language_card_paging();

			insert_media(target.media);
		}

		~ConcreteMachine() {
			audio_queue_.flush();
		}

		void setup_output(float aspect_ratio) override {
			video_.reset(new AppleII::Video::Video<VideoBusHandler>(video_bus_handler_));
			video_->set_character_rom(character_rom_);
		}

		void close_output() override {
			video_.reset();
		}

		Outputs::CRT::CRT *get_crt() override {
			return video_->get_crt();
		}

		Outputs::Speaker::Speaker *get_speaker() override {
			return &speaker_;
		}

		forceinline Cycles perform_bus_operation(const CPU::MOS6502::BusOperation operation, const uint16_t address, uint8_t *const value) {
			++ cycles_since_video_update_;
			++ cycles_since_card_update_;
			cycles_since_audio_update_ += Cycles(7);

			// The Apple II has a slightly weird timing pattern: every 65th CPU cycle is stretched
			// by an extra 1/7th. That's because one cycle lasts 3.5 NTSC colour clocks, so after
			// 65 cycles a full line of 227.5 colour clocks have passed. But the high-rate binary
			// signal approximation that produces colour needs to be in phase, so a stretch of exactly
			// 0.5 further colour cycles is added. The video class handles that implicitly, but it
			// needs to be accumulated here for the audio.
			cycles_into_current_line_ = (cycles_into_current_line_ + 1) % 65;
			const bool is_stretched_cycle = !cycles_into_current_line_;
			if(is_stretched_cycle) {
				++ cycles_since_audio_update_;
				++ stretched_cycles_since_card_update_;
			}

			bool has_updated_cards = false;
			if(read_pages_[address >> 8]) {
				if(isReadOperation(operation)) *value = read_pages_[address >> 8][address & 0xff];
				else if(write_pages_[address >> 8]) write_pages_[address >> 8][address & 0xff] = *value;

				if(should_load_quickly_) {
					// Check for a prima facie entry into RWTS.
					if(operation == CPU::MOS6502::BusOperation::ReadOpcode && address == 0xb7b5) {
						// Grab the IO control block address for inspection.
						uint16_t io_control_block_address =
							static_cast<uint16_t>(
								(m6502_.get_value_of_register(CPU::MOS6502::Register::A) << 8) |
								m6502_.get_value_of_register(CPU::MOS6502::Register::Y)
							);

						// Verify that this is table type one, for execution on card six,
						// against drive 1 or 2, and that the command is either a seek or a sector read.
						if(
							ram_[io_control_block_address+0x00] == 0x01 &&
							ram_[io_control_block_address+0x01] == 0x60 &&
							ram_[io_control_block_address+0x02] > 0 && ram_[io_control_block_address+0x02] < 3 &&
							ram_[io_control_block_address+0x0c] < 2
						) {
							const uint8_t iob_track = ram_[io_control_block_address+4];
							const uint8_t iob_sector = ram_[io_control_block_address+5];
							const uint8_t iob_drive = ram_[io_control_block_address+2] - 1;

							// Get the track identified and store the new head position.
							auto track = diskii_card()->get_drive(iob_drive).step_to(Storage::Disk::HeadPosition(iob_track));

							// DOS 3.3 keeps the current track (unspecified drive) in 0x478; the current track for drive 1 and drive 2
							// is also kept in that Disk II card's screen hole.
							ram_[0x478] = iob_track;
							if(ram_[io_control_block_address+0x02] == 1) {
								ram_[0x47e] = iob_track;
							} else {
								ram_[0x4fe] = iob_track;
							}

							// Check whether this is a read, not merely a seek.
							if(ram_[io_control_block_address+0x0c] == 1) {
								// Apple the DOS 3.3 formula to map the requested logical sector to a physical sector.
								const int physical_sector = (iob_sector == 15) ? 15 : ((iob_sector * 13) % 15);

								// Parse the entire track. TODO: cache these.
								auto sector_map = Storage::Encodings::AppleGCR::sectors_from_segment(
									Storage::Disk::track_serialisation(*track, Storage::Time(1, 50000)));

								bool found_sector = false;
								for(const auto &pair: sector_map) {
									if(pair.second.address.sector == physical_sector) {
										found_sector = true;

										// Copy the sector contents to their destination.
										uint16_t target = static_cast<uint16_t>(
											ram_[io_control_block_address+8] |
											(ram_[io_control_block_address+9] << 8)
										);

										for(size_t c = 0; c < 256; ++c) {
											ram_[target] = pair.second.data[c];
											++target;
										}

										// Set no error encountered.
										ram_[io_control_block_address + 0xd] = 0;
										break;
									}
								}

								if(found_sector) {
									// Set no error in the flags register too, and RTS.
									m6502_.set_value_of_register(CPU::MOS6502::Register::Flags, m6502_.get_value_of_register(CPU::MOS6502::Register::Flags) & ~1);
									*value = 0x60;
								}
							} else {
								// No error encountered; RTS.
								m6502_.set_value_of_register(CPU::MOS6502::Register::Flags, m6502_.get_value_of_register(CPU::MOS6502::Register::Flags) & ~1);
								*value = 0x60;
							}
						}
					}
				}
			} else {
				// Assume a vapour read unless it turns out otherwise; this is a little
				// wasteful but works for now.
				//
				// Longer version: like many other machines, when the Apple II reads from
				// an address at which no hardware loads the data bus, through a process of
				// practical analogue effects it'll end up receiving whatever was last on
				// the bus. Which will always be whatever the video circuit fetched because
				// that fetches in between every instruction.
				//
				// So this code assumes that'll happen unless it later determines that it
				// doesn't. The call into the video isn't free because it's a just-in-time
				// actor, but this will actually be the result most of the time so it's not
				// too terrible.
				if(isReadOperation(operation) && address != 0xc000) {
					*value = video_->get_last_read_value(cycles_since_video_update_);
				}

				switch(address) {
					default:
						if(isReadOperation(operation)) {
							// Read-only switches.
							switch(address) {
								default:
									printf("Unknown (?) read from %04x\n", address);
								break;

								case 0xc000:
									if(string_serialiser_) {
										*value = string_serialiser_->head() | 0x80;
									} else {
										*value = keyboard_input_;
									}
								break;

								case 0xc061:	// Switch input 0.
									*value &= 0x7f;
									if(static_cast<Joystick *>(joysticks_[0].get())->buttons[0] || static_cast<Joystick *>(joysticks_[1].get())->buttons[2])
										*value |= 0x80;
								break;
								case 0xc062:	// Switch input 1.
									*value &= 0x7f;
									if(static_cast<Joystick *>(joysticks_[0].get())->buttons[1] || static_cast<Joystick *>(joysticks_[1].get())->buttons[1])
										*value |= 0x80;
								break;
								case 0xc063:	// Switch input 2.
									*value &= 0x7f;
									if(static_cast<Joystick *>(joysticks_[0].get())->buttons[2] || static_cast<Joystick *>(joysticks_[1].get())->buttons[0])
										*value |= 0x80;
								break;

								case 0xc064:	// Analogue input 0.
								case 0xc065:	// Analogue input 1.
								case 0xc066:	// Analogue input 2.
								case 0xc067: {	// Analogue input 3.
									const size_t input = address - 0xc064;
									*value &= 0x7f;
									if(analogue_channel_is_discharged(input)) {
										*value |= 0x80;
									}
								} break;

								// The IIe-only state reads follow...
								case 0xc011:	if(is_iie) *value = (*value & 0x7f) | language_card_.bank1 ? 0x80 : 0x00;						break;
								case 0xc012:	if(is_iie) *value = (*value & 0x7f) | language_card_.read ? 0x80 : 0x00;						break;
								case 0xc013:	if(is_iie) *value = (*value & 0x7f) | read_auxiliary_memory_ ? 0x80 : 0x00;						break;
								case 0xc014:	if(is_iie) *value = (*value & 0x7f) | write_auxiliary_memory_ ? 0x80 : 0x00;					break;
								case 0xc015:	if(is_iie) *value = (*value & 0x7f) | internal_CX_rom_ ? 0x80 : 0x00;							break;
								case 0xc016:	if(is_iie) *value = (*value & 0x7f) | alternative_zero_page_ ? 0x80 : 0x00;						break;
								case 0xc017:	if(is_iie) *value = (*value & 0x7f) | slot_C3_rom_ ? 0x80 : 0x00;								break;
								case 0xc018:	if(is_iie) *value = (*value & 0x7f) | video_->get_80_store() ? 0x80 : 0x00;						break;
								case 0xc01a:	if(is_iie) *value = (*value & 0x7f) | video_->get_text() ? 0x80 : 0x00;							break;
								case 0xc01b:	if(is_iie) *value = (*value & 0x7f) | video_->get_mixed() ? 0x80 : 0x00;						break;
								case 0xc01c:	if(is_iie) *value = (*value & 0x7f) | video_->get_page2() ? 0x80 : 0x00;						break;
								case 0xc01d:	if(is_iie) *value = (*value & 0x7f) | video_->get_high_resolution() ? 0x80 : 0x00;				break;
								case 0xc01e:	if(is_iie) *value = (*value & 0x7f) | video_->get_alternative_character_set() ? 0x80 : 0x00;	break;
								case 0xc01f:	if(is_iie) *value = (*value & 0x7f) | video_->get_80_columns() ? 0x80 : 0x00;					break;
								case 0xc07f:	if(is_iie) *value = (*value & 0x7f) | video_->get_double_high_resolution() ? 0x80 : 0x00;		break;
							}
						} else {
							// Write-only switches. All IIe as currently implemented.
							if(is_iie) {
								if(address >= 0xc000 && address < 0xc100) printf("w %04x\n", address);
								switch(address) {
									default:
										printf("Unknown (?) write to %04x\n", address);
									break;

									case 0xc002:
									case 0xc003:
										read_auxiliary_memory_ = !!(address&1);
										set_main_paging();
									break;
									case 0xc004:
									case 0xc005:
										write_auxiliary_memory_ = !!(address&1);
										set_main_paging();
									break;

									case 0xc006:
									case 0xc007:
										internal_CX_rom_ = !!(address&1);
										set_card_paging();
									break;
									case 0xc00a:
									case 0xc00b:
										slot_C3_rom_ = !!(address&1);
										set_card_paging();
									break;

									case 0xc00e:
									case 0xc00f:	video_->set_alternative_character_set(!!(address&1));	break;

									case 0xc00c:
									case 0xc00d:	video_->set_80_columns(!!(address&1));					break;

									case 0xc000:
									case 0xc001:
										video_->set_80_store(!!(address&1));
										set_main_paging();
									break;

									case 0xc05e:
									case 0xc05f:	video_->set_double_high_resolution(!(address&1));		break;

									case 0xc008:
									case 0xc009:
										// The alternative zero page setting affects both bank 0 and any RAM
										// that's paged as though it were on a language card.
										alternative_zero_page_ = !!(address&1);
										if(alternative_zero_page_) {
											read_pages_[0] = aux_ram_;
										} else {
											read_pages_[0] = ram_;
										}
										read_pages_[1] = read_pages_[0] + 256;
										write_pages_[0] = read_pages_[0];
										write_pages_[1] = read_pages_[1];
										set_language_card_paging();
									break;
								}
							}
						}
					break;

					case 0xc070: {	// Permit analogue inputs that are currently discharged to begin a charge cycle.
									// Ensure those that were still charging retain that state.
						for(size_t c = 0; c < 4; ++c) {
							if(analogue_channel_is_discharged(c)) {
								analogue_biases_[c] = 0.0f;
							} else {
								analogue_biases_[c] += analogue_charge_;
							}
						}
						analogue_charge_ = 0.0f;
					} break;

					/* Read-write switches. */
					case 0xc050:	update_video();		video_->set_text(false);			break;
					case 0xc051:	update_video();		video_->set_text(true);				break;
					case 0xc052:	update_video();		video_->set_mixed(false);			break;
					case 0xc053:	update_video();		video_->set_mixed(true);			break;
					case 0xc054:
					case 0xc055:
						update_video();
						video_->set_page2(!!(address&1));
						set_main_paging();
					break;
					case 0xc056:	update_video();		video_->set_high_resolution(false);	break;
					case 0xc057:	update_video();		video_->set_high_resolution(true);	break;

					case 0xc010:
						keyboard_input_ &= 0x7f;
						if(string_serialiser_) {
							if(!string_serialiser_->advance())
								string_serialiser_.reset();
						}

						// On the IIe, reading C010 returns additional key info.
						if(is_iie && isReadOperation(operation)) {
							// TODO!
							*value = 0;
						}
					break;

					case 0xc030:
						update_audio();
						audio_toggle_.set_output(!audio_toggle_.get_output());
					break;

					case 0xc080: case 0xc084: case 0xc088: case 0xc08c:
					case 0xc081: case 0xc085: case 0xc089: case 0xc08d:
					case 0xc082: case 0xc086: case 0xc08a: case 0xc08e:
					case 0xc083: case 0xc087: case 0xc08b: case 0xc08f:
						// Quotes below taken from Understanding the Apple II, p. 5-28 and 5-29.

						// "A3 controls the 4K bank selection"
						language_card_.bank1 = (address&8);

						// "Access to $C080, $C083, $C084, $0087, $C088, $C08B, $C08C, or $C08F sets the READ ENABLE flip-flop"
						// (other accesses reset it)
						language_card_.read = !(((address&2) >> 1) ^ (address&1));

						// "The WRITE ENABLE' flip-flop is reset by an odd read access to the $C08X range when the PRE-WRITE flip-flop is set."
						if(language_card_.pre_write && isReadOperation(operation) && (address&1)) language_card_.write = false;

						// "[The WRITE ENABLE' flip-flop] is set by an even access in the $C08X range."
						if(!(address&1)) language_card_.write = true;

						// ("Any other type of access causes the WRITE ENABLE' flip-flop to hold its current state.")

						// "The PRE-WRITE flip-flop is set by an odd read access in the $C08X range. It is reset by an even access or a write access."
						language_card_.pre_write = isReadOperation(operation) ? (address&1) : false;

						// Apply whatever the net effect of all that is to the memory map.
						set_language_card_paging();
					break;
				}

				/*
					Communication with cards follows.
				*/

				if(!read_pages_[address >> 8] && address >= 0xc090 && address < 0xc800) {
					// If this is a card access, figure out which card is at play before determining
					// the totality of who needs messaging.
					size_t card_number = 0;
					AppleII::Card::Select select = AppleII::Card::None;

					if(address >= 0xc100) {
						/*
							Decode the area conventionally used by cards for ROMs:
								0xCn00 to 0xCnff: card n.
						*/
						card_number = (address - 0xc100) >> 8;
						select = AppleII::Card::Device;
					} else {
						/*
							Decode the area conventionally used by cards for registers:
								C0n0 to C0nF: card n - 8.
						*/
						card_number = (address - 0xc090) >> 4;
						select = AppleII::Card::IO;
					}

					// If the selected card is a just-in-time card, update the just-in-time cards,
					// and then message it specifically.
					const bool is_read = isReadOperation(operation);
					AppleII::Card *const target = cards_[static_cast<size_t>(card_number)].get();
					if(target && !is_every_cycle_card(target)) {
						update_just_in_time_cards();
						target->perform_bus_operation(select, is_read, address, value);
					}

					// Update all the every-cycle cards regardless, but send them a ::None select if they're
					// not the one actually selected.
					for(const auto &card: every_cycle_cards_) {
						card->run_for(Cycles(1), is_stretched_cycle);
						card->perform_bus_operation(
							(card == target) ? select : AppleII::Card::None,
							is_read, address, value);
					}
					has_updated_cards = true;
				}
			}

			if(!has_updated_cards && !every_cycle_cards_.empty()) {
				// Update all every-cycle cards and give them the cycle.
				const bool is_read = isReadOperation(operation);
				for(const auto &card: every_cycle_cards_) {
					card->run_for(Cycles(1), is_stretched_cycle);
					card->perform_bus_operation(AppleII::Card::None, is_read, address, value);
				}
			}

			// Update analogue charge level.
			analogue_charge_ = std::min(analogue_charge_ + 1.0f / 2820.0f, 1.1f);

			return Cycles(1);
		}

		void flush() {
			update_video();
			update_audio();
			update_just_in_time_cards();
			audio_queue_.perform();
		}

		void run_for(const Cycles cycles) override {
			m6502_.run_for(cycles);
		}

		void set_key_pressed(Key key, char value, bool is_pressed) override {
			if(key == Key::F12) {
				m6502_.set_reset_line(is_pressed);
				return;
			}

			if(is_pressed) {
				// If no ASCII value is supplied, look for a few special cases.
				if(!value) {
					switch(key) {
						case Key::Left:			value = 0x08;	break;
						case Key::Right:		value = 0x15;	break;
						case Key::Down:			value = 0x0a;	break;
						case Key::Up:			value = 0x0b;	break;
						case Key::BackSpace:	value = 0x7f;	break;
						default: break;
					}
				}

				keyboard_input_ = static_cast<uint8_t>(toupper(value) | 0x80);
			}
		}

		Inputs::Keyboard &get_keyboard() override {
			return *this;
		}

		void type_string(const std::string &string) override {
			string_serialiser_.reset(new Utility::StringSerialiser(string, true));
		}

		// MARK: MediaTarget
		bool insert_media(const Analyser::Static::Media &media) override {
			if(!media.disks.empty()) {
				auto diskii = diskii_card();
				if(diskii) diskii->set_disk(media.disks[0], 0);
			}
			return true;
		}

		// MARK: Activity::Source
		void set_activity_observer(Activity::Observer *observer) override {
			for(const auto &card: cards_) {
				if(card) card->set_activity_observer(observer);
			}
		}

		// MARK: Options
		std::vector<std::unique_ptr<Configurable::Option>> get_options() override {
			return AppleII::get_options();
		}

		void set_selections(const Configurable::SelectionSet &selections_by_option) override {
			bool quickload;
			if(Configurable::get_quick_load_tape(selections_by_option, quickload)) {
				should_load_quickly_ = quickload;
			}
		}

		Configurable::SelectionSet get_accurate_selections() override {
			Configurable::SelectionSet selection_set;
			Configurable::append_quick_load_tape_selection(selection_set, false);
			return selection_set;
		}

		Configurable::SelectionSet get_user_friendly_selections() override {
			Configurable::SelectionSet selection_set;
			Configurable::append_quick_load_tape_selection(selection_set, true);
			return selection_set;
		}

		// MARK: JoystickMachine
		std::vector<std::unique_ptr<Inputs::Joystick>> &get_joysticks() override {
			return joysticks_;
		}
};

}

using namespace AppleII;

Machine *Machine::AppleII(const Analyser::Static::Target *target, const ROMMachine::ROMFetcher &rom_fetcher) {
	using Target = Analyser::Static::AppleII::Target;
	const Target *const appleii_target = dynamic_cast<const Target *>(target);
	if(appleii_target->model == Target::Model::IIe) {
		return new ConcreteMachine<true>(*appleii_target, rom_fetcher);
	} else {
		return new ConcreteMachine<false>(*appleii_target, rom_fetcher);
	}
}

Machine::~Machine() {}
