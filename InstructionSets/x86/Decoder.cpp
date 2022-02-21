//
//  x86.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 01/01/21.
//  Copyright © 2021 Thomas Harte. All rights reserved.
//

#include "Decoder.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

using namespace InstructionSet::x86;

template <Model model>
std::pair<int, typename Decoder<model>::InstructionT> Decoder<model>::decode(const uint8_t *source, size_t length) {
	const uint8_t *const end = source + length;

	// MARK: - Prefixes (if present) and the opcode.

/// Helper macro for those that follow.
#define SetOpSrcDestSize(op, src, dest, size)	\
	operation_ = Operation::op;			\
	source_ = Source::src;				\
	destination_ = Source::dest;		\
	operation_size_ = size

/// Covers anything which is complete as soon as the opcode is encountered.
#define Complete(op, src, dest, size)		\
	SetOpSrcDestSize(op, src, dest, size);	\
	phase_ = Phase::ReadyToPost

/// Handles instructions of the form rr, kk and rr, jjkk, i.e. a destination register plus an operand.
#define RegData(op, dest, size)							\
	SetOpSrcDestSize(op, DirectAddress, dest, size);	\
	source_ = Source::Immediate;						\
	operand_size_ = size;								\
	phase_ = Phase::DisplacementOrOperand

/// Handles instructions of the form Ax, jjkk where the latter is implicitly an address.
#define RegAddr(op, dest, op_size, addr_size)			\
	SetOpSrcDestSize(op, DirectAddress, dest, op_size);	\
	operand_size_ = addr_size;							\
	phase_ = Phase::DisplacementOrOperand

/// Handles instructions of the form jjkk, Ax where the former is implicitly an address.
#define AddrReg(op, source, op_size, addr_size)				\
	SetOpSrcDestSize(op, source, DirectAddress, op_size);	\
	operand_size_ = addr_size;								\
	destination_ = Source::DirectAddress;					\
	phase_ = Phase::DisplacementOrOperand

/// Covers both `mem/reg, reg` and `reg, mem/reg`.
#define MemRegReg(op, format, size)				\
	operation_ = Operation::op;					\
	phase_ = Phase::ModRegRM;					\
	modregrm_format_ = ModRegRMFormat::format;	\
	operand_size_ = 0;							\
	operation_size_ = size

/// Handles JO, JNO, JB, etc — jumps with a single byte displacement.
#define Jump(op)									\
	operation_ = Operation::op;						\
	phase_ = Phase::DisplacementOrOperand;			\
	displacement_size_ = 1

/// Handles far CALL and far JMP — fixed four byte operand operations.
#define Far(op)										\
	operation_ = Operation::op;						\
	phase_ = Phase::DisplacementOrOperand;			\
	operand_size_ = 4;								\

/// Handles ENTER — a fixed three-byte operation.
#define Displacement16Operand8(op)					\
	operation_ = Operation::op;						\
	phase_ = Phase::DisplacementOrOperand;			\
	displacement_size_ = 2;							\
	operand_size_ = 1;								\

#define undefined()	{												\
	const auto result = std::make_pair(consumed_, InstructionT());	\
	reset_parsing();												\
	return result;													\
}

	while(phase_ == Phase::Instruction && source != end) {
		// Retain the instruction byte, in case additional decoding is deferred
		// to the ModRegRM byte.
		instr_ = *source;
		++source;
		++consumed_;

		switch(instr_) {
			default: undefined();

#define PartialBlock(start, operation)								\
	case start + 0x00: MemRegReg(operation, MemReg_Reg, 1);	break;	\
	case start + 0x01: MemRegReg(operation, MemReg_Reg, 2);	break;	\
	case start + 0x02: MemRegReg(operation, Reg_MemReg, 1);	break;	\
	case start + 0x03: MemRegReg(operation, Reg_MemReg, 2);	break;	\
	case start + 0x04: RegData(operation, eAX, 1);			break;	\
	case start + 0x05: RegData(operation, eAX, 2)

			PartialBlock(0x00, ADD);					break;
			case 0x06: Complete(PUSH, ES, None, 2);		break;
			case 0x07: Complete(POP, None, ES, 2);		break;

			PartialBlock(0x08, OR);						break;
			case 0x0e: Complete(PUSH, CS, None, 2);		break;

			// The 286 onwards have a further set of instructions
			// prefixed with $0f.
			case 0x0f:
				if constexpr (model < Model::i80286) undefined();
				phase_ = Phase::InstructionPageF;
			break;

			PartialBlock(0x10, ADC);					break;
			case 0x16: Complete(PUSH, SS, None, 2);		break;
			case 0x17: Complete(POP, None, SS, 2);		break;

			PartialBlock(0x18, SBB);					break;
			case 0x1e: Complete(PUSH, DS, None, 2);		break;
			case 0x1f: Complete(POP, None, DS, 2);		break;

			PartialBlock(0x20, AND);					break;
			case 0x26: segment_override_ = Source::ES;	break;
			case 0x27: Complete(DAA, eAX, eAX, 1);		break;

			PartialBlock(0x28, SUB);					break;
			case 0x2e: segment_override_ = Source::CS;	break;
			case 0x2f: Complete(DAS, eAX, eAX, 1);		break;

			PartialBlock(0x30, XOR);					break;
			case 0x36: segment_override_ = Source::SS;	break;
			case 0x37: Complete(AAA, eAX, eAX, 2);		break;

			PartialBlock(0x38, CMP);					break;
			case 0x3e: segment_override_ = Source::DS;	break;
			case 0x3f: Complete(AAS, eAX, eAX, 2);		break;

#undef PartialBlock

#define RegisterBlock(start, operation)								\
	case start + 0x00: Complete(operation, eAX, eAX, 2);	break;	\
	case start + 0x01: Complete(operation, eCX, eCX, 2);	break;	\
	case start + 0x02: Complete(operation, eDX, eDX, 2);	break;	\
	case start + 0x03: Complete(operation, eBX, eBX, 2);	break;	\
	case start + 0x04: Complete(operation, eSP, eSP, 2);	break;	\
	case start + 0x05: Complete(operation, eBP, eBP, 2);	break;	\
	case start + 0x06: Complete(operation, eSI, eSI, 2);	break;	\
	case start + 0x07: Complete(operation, eDI, eDI, 2)

			RegisterBlock(0x40, INC);	break;
			RegisterBlock(0x48, DEC);	break;
			RegisterBlock(0x50, PUSH);	break;
			RegisterBlock(0x58, POP);	break;

#undef RegisterBlock

			case 0x60:
				if constexpr (model < Model::i80186) undefined();
				Complete(PUSHA, None, None, 2);
			break;
			case 0x61:
				if constexpr (model < Model::i80186) undefined();
				Complete(POPA, None, None, 2);
			break;
			case 0x62:
				if constexpr (model < Model::i80186) undefined();
				MemRegReg(BOUND, Reg_MemReg, 2);
			break;
			case 0x63:
				if constexpr (model < Model::i80286) undefined();
				MemRegReg(ARPL, MemReg_Reg, 2);
			break;
			case 0x6c:	// INSB
				if constexpr (model < Model::i80186) undefined();
				Complete(INS, None, None, 1);
			break;
			case 0x6d:	// INSW
				if constexpr (model < Model::i80186) undefined();
				Complete(INS, None, None, 2);
			break;
			case 0x6e:	// OUTSB
				if constexpr (model < Model::i80186) undefined();
				Complete(OUTS, None, None, 1);
			break;
			case 0x6f:	// OUTSW
				if constexpr (model < Model::i80186) undefined();
				Complete(OUTS, None, None, 2);
			break;

			case 0x70: Jump(JO);	break;
			case 0x71: Jump(JNO);	break;
			case 0x72: Jump(JB);	break;
			case 0x73: Jump(JNB);	break;
			case 0x74: Jump(JE);	break;
			case 0x75: Jump(JNE);	break;
			case 0x76: Jump(JBE);	break;
			case 0x77: Jump(JNBE);	break;
			case 0x78: Jump(JS);	break;
			case 0x79: Jump(JNS);	break;
			case 0x7a: Jump(JP);	break;
			case 0x7b: Jump(JNP);	break;
			case 0x7c: Jump(JL);	break;
			case 0x7d: Jump(JNL);	break;
			case 0x7e: Jump(JLE);	break;
			case 0x7f: Jump(JNLE);	break;

			case 0x80: MemRegReg(Invalid, MemRegADD_to_CMP, 1);	break;
			case 0x81: MemRegReg(Invalid, MemRegADD_to_CMP, 2);	break;
			case 0x82: MemRegReg(Invalid, MemRegADC_to_CMP, 1);	break;
			case 0x83: MemRegReg(Invalid, MemRegADC_to_CMP, 2);	break;

			case 0x84: MemRegReg(TEST, MemReg_Reg, 1);	break;
			case 0x85: MemRegReg(TEST, MemReg_Reg, 2);	break;
			case 0x86: MemRegReg(XCHG, Reg_MemReg, 1);	break;
			case 0x87: MemRegReg(XCHG, Reg_MemReg, 2);	break;
			case 0x88: MemRegReg(MOV, MemReg_Reg, 1);	break;
			case 0x89: MemRegReg(MOV, MemReg_Reg, 2);	break;
			case 0x8a: MemRegReg(MOV, Reg_MemReg, 1);	break;
			case 0x8b: MemRegReg(MOV, Reg_MemReg, 2);	break;
			// 0x8c: not used.
			case 0x8d: MemRegReg(LEA, Reg_MemReg, 2);	break;
			case 0x8e: MemRegReg(MOV, SegReg, 2);		break;
			case 0x8f: MemRegReg(POP, MemRegPOP, 2);	break;

			case 0x90: Complete(NOP, None, None, 0);	break;	// Or XCHG AX, AX?
			case 0x91: Complete(XCHG, eAX, eCX, 2);		break;
			case 0x92: Complete(XCHG, eAX, eDX, 2);		break;
			case 0x93: Complete(XCHG, eAX, eBX, 2);		break;
			case 0x94: Complete(XCHG, eAX, eSP, 2);		break;
			case 0x95: Complete(XCHG, eAX, eBP, 2);		break;
			case 0x96: Complete(XCHG, eAX, eSI, 2);		break;
			case 0x97: Complete(XCHG, eAX, eDI, 2);		break;

			case 0x98: Complete(CBW, eAX, AH, 1);		break;
			case 0x99: Complete(CWD, eAX, eDX, 2);		break;
			case 0x9a: Far(CALLF);						break;
			case 0x9b: Complete(WAIT, None, None, 0);	break;
			case 0x9c: Complete(PUSHF, None, None, 2);	break;
			case 0x9d: Complete(POPF, None, None, 2);	break;
			case 0x9e: Complete(SAHF, None, None, 1);	break;
			case 0x9f: Complete(LAHF, None, None, 1);	break;

			case 0xa0: RegAddr(MOV, eAX, 1, 1);	break;
			case 0xa1: RegAddr(MOV, eAX, 2, 2);	break;
			case 0xa2: AddrReg(MOV, eAX, 1, 1);	break;
			case 0xa3: AddrReg(MOV, eAX, 2, 2);	break;

			case 0xa4: Complete(MOVS, None, None, 1);	break;
			case 0xa5: Complete(MOVS, None, None, 2);	break;
			case 0xa6: Complete(CMPS, None, None, 1);	break;
			case 0xa7: Complete(CMPS, None, None, 2);	break;
			case 0xa8: RegData(TEST, eAX, 1);			break;
			case 0xa9: RegData(TEST, eAX, 2);			break;
			case 0xaa: Complete(STOS, None, None, 1);	break;
			case 0xab: Complete(STOS, None, None, 2);	break;
			case 0xac: Complete(LODS, None, None, 1);	break;
			case 0xad: Complete(LODS, None, None, 2);	break;
			case 0xae: Complete(SCAS, None, None, 1);	break;
			case 0xaf: Complete(SCAS, None, None, 2);	break;

			case 0xb0: RegData(MOV, eAX, 1);	break;
			case 0xb1: RegData(MOV, eCX, 1);	break;
			case 0xb2: RegData(MOV, eDX, 1);	break;
			case 0xb3: RegData(MOV, eBX, 1);	break;
			case 0xb4: RegData(MOV, AH, 1);		break;
			case 0xb5: RegData(MOV, CH, 1);		break;
			case 0xb6: RegData(MOV, DH, 1);		break;
			case 0xb7: RegData(MOV, BH, 1);		break;
			case 0xb8: RegData(MOV, eAX, 2);	break;
			case 0xb9: RegData(MOV, eCX, 2);	break;
			case 0xba: RegData(MOV, eDX, 2);	break;
			case 0xbb: RegData(MOV, eBX, 2);	break;
			case 0xbc: RegData(MOV, eSP, 2);	break;
			case 0xbd: RegData(MOV, eBP, 2);	break;
			case 0xbe: RegData(MOV, eSI, 2);	break;
			case 0xbf: RegData(MOV, eDI, 2);	break;

			case 0xc2: RegData(RETN, None, 2);			break;
			case 0xc3: Complete(RETN, None, None, 2);	break;
			case 0xc4: MemRegReg(LES, Reg_MemReg, 2);	break;
			case 0xc5: MemRegReg(LDS, Reg_MemReg, 2);	break;
			case 0xc6: MemRegReg(MOV, MemRegMOV, 1);	break;
			case 0xc7: MemRegReg(MOV, MemRegMOV, 2);	break;

			case 0xc8:
				if constexpr (model < Model::i80186) undefined();
				Displacement16Operand8(ENTER);
			break;
			case 0xc9:
				if constexpr (model < Model::i80186) undefined();
				Complete(LEAVE, None, None, 0);
			break;

			case 0xca: RegData(RETF, None, 2);			break;
			case 0xcb: Complete(RETF, None, None, 4);	break;

			case 0xcc: Complete(INT3, None, None, 0);	break;
			case 0xcd: RegData(INT, None, 1);			break;
			case 0xce: Complete(INTO, None, None, 0);	break;
			case 0xcf: Complete(IRET, None, None, 0);	break;

			case 0xd0: case 0xd1:
				phase_ = Phase::ModRegRM;
				modregrm_format_ = ModRegRMFormat::MemRegROL_to_SAR;
				operation_size_ = 1 + (instr_ & 1);
				source_ = Source::Immediate;
				operand_ = 1;
			break;
			case 0xd2: case 0xd3:
				phase_ = Phase::ModRegRM;
				modregrm_format_ = ModRegRMFormat::MemRegROL_to_SAR;
				operation_size_ = 1 + (instr_ & 1);
				source_ = Source::eCX;
			break;
			case 0xd4: RegData(AAM, eAX, 1);				break;
			case 0xd5: RegData(AAD, eAX, 1);				break;

			case 0xd7: Complete(XLAT, None, None, 1);	break;

			case 0xd8: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xd9: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xda: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xdb: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xdc: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xdd: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xde: MemRegReg(ESC, MemReg_Reg, 0);	break;
			case 0xdf: MemRegReg(ESC, MemReg_Reg, 0);	break;

			case 0xe0: Jump(LOOPNE);	break;
			case 0xe1: Jump(LOOPE);		break;
			case 0xe2: Jump(LOOP);		break;
			case 0xe3: Jump(JPCX);		break;

			case 0xe4: RegAddr(IN, eAX, 1, 1);	break;
			case 0xe5: RegAddr(IN, eAX, 2, 1);	break;
			case 0xe6: AddrReg(OUT, eAX, 1, 1);	break;
			case 0xe7: AddrReg(OUT, eAX, 2, 1);	break;

			case 0xe8: RegData(CALLD, None, 2);	break;
			case 0xe9: RegData(JMPN, None, 2);	break;
			case 0xea: Far(JMPF);				break;
			case 0xeb: Jump(JMPN);				break;

			case 0xec: Complete(IN, eDX, eAX, 1);	break;
			case 0xed: Complete(IN, eDX, eAX, 1);		break;
			case 0xee: Complete(OUT, eAX, eDX, 1);	break;
			case 0xef: Complete(OUT, eAX, eDX, 2);	break;

			case 0xf0: lock_ = true;					break;
			case 0xf2: repetition_ = Repetition::RepNE;	break;
			case 0xf3: repetition_ = Repetition::RepE;	break;

			case 0xf4: Complete(HLT, None, None, 1);				break;
			case 0xf5: Complete(CMC, None, None, 1);				break;
			case 0xf6: MemRegReg(Invalid, MemRegTEST_to_IDIV, 1);	break;
			case 0xf7: MemRegReg(Invalid, MemRegTEST_to_IDIV, 2);	break;

			case 0xf8: Complete(CLC, None, None, 1);	break;
			case 0xf9: Complete(STC, None, None, 1);	break;
			case 0xfa: Complete(CLI, None, None, 1);	break;
			case 0xfb: Complete(STI, None, None, 1);	break;
			case 0xfc: Complete(CLD, None, None, 1);	break;
			case 0xfd: Complete(STD, None, None, 1);	break;

			case 0xfe: MemRegReg(Invalid, MemRegINC_DEC, 1);		break;
			case 0xff: MemRegReg(Invalid, MemRegINC_to_PUSH, 1);	break;
		}
	}

	// MARK: - Additional F page of instructions.
	if(phase_ == Phase::InstructionPageF && source != end) {
		// Update the instruction acquired.
		instr_ = 0x0f00 | *source;
		++source;
		++consumed_;

		// NB: to reach here, the instruction set must be at least
		// that of an 80286.
		switch(instr_) {
			default: undefined();

			case 0x00:	MemRegReg(Invalid, MemRegSLDT_to_VERW, 2);	break;
			case 0x01:	MemRegReg(Invalid, MemRegSGDT_to_LMSW, 2);	break;
			case 0x02:	MemRegReg(LAR, Reg_MemReg, 2);				break;
			case 0x03:	MemRegReg(LSL, Reg_MemReg, 2);				break;
			case 0x05:
				if constexpr (model != Model::i80286) undefined();
				Complete(LOADALL, None, None, 0);
			break;
			case 0x06:	Complete(CLTS, None, None, 1);				break;
		}
	}

#undef Far
#undef Jump
#undef MemRegReg
#undef AddrReg
#undef RegAddr
#undef RegData
#undef Complete
#undef SetOpSrcDestSize

	// MARK: - ModRegRM byte, if any.

	if(phase_ == Phase::ModRegRM && source != end) {
		const uint8_t mod = *source >> 6;		// i.e. mode.
		const uint8_t reg = (*source >> 3) & 7;	// i.e. register.
		const uint8_t rm = *source & 7;			// i.e. register/memory.
		++source;
		++consumed_;

		Source memreg;
		constexpr Source reg_table[3][8] = {
			{},
			{
				Source::eAX,	Source::eCX,	Source::eDX,	Source::eBX,
				Source::AH,		Source::CH,		Source::DH,		Source::BH,
			}, {
				Source::eAX,	Source::eCX,	Source::eDX,	Source::eBX,
				Source::eSP,	Source::eBP,	Source::eSI,	Source::eDI,
			}
		};
		switch(mod) {
			default:
				displacement_size_ = 1 + (mod == 2);
				[[fallthrough]];
			case 0: {
				constexpr ScaleIndexBase rm_table[8] = {
					ScaleIndexBase(0, Source::eBX, Source::eSI),
					ScaleIndexBase(0, Source::eBX, Source::eDI),
					ScaleIndexBase(0, Source::eBP, Source::eSI),
					ScaleIndexBase(0, Source::eBP, Source::eDI),
					ScaleIndexBase(0, Source::None, Source::eSI),
					ScaleIndexBase(0, Source::None, Source::eDI),
					ScaleIndexBase(0, Source::None, Source::eBP),
					ScaleIndexBase(0, Source::None, Source::eBX),
				};

				memreg = Source::Indirect;
				sib_ = rm_table[rm];
			} break;

			// Other operand is just a register.
			case 3:
				memreg = reg_table[operation_size_][rm];

				// LES and LDS accept a memory argument only, not a register.
				if(operation_ == Operation::LES || operation_ == Operation::LDS) {
					undefined();
				}
			break;
		}

		switch(modregrm_format_) {
			case ModRegRMFormat::Reg_MemReg:
			case ModRegRMFormat::MemReg_Reg: {
				if(modregrm_format_ == ModRegRMFormat::Reg_MemReg) {
					source_ = memreg;
					destination_ = reg_table[operation_size_][reg];
				} else {
					source_ = reg_table[operation_size_][reg];
					destination_ = memreg;
				}
			} break;

			case ModRegRMFormat::MemRegTEST_to_IDIV:
				source_ = destination_ = memreg;

				switch(reg) {
					default: undefined();

					case 0: 	operation_ = Operation::TEST;	break;
					case 2: 	operation_ = Operation::NOT;	break;
					case 3: 	operation_ = Operation::NEG;	break;
					case 4: 	operation_ = Operation::MUL;	break;
					case 5: 	operation_ = Operation::IMUL;	break;
					case 6: 	operation_ = Operation::DIV;	break;
					case 7: 	operation_ = Operation::IDIV;	break;
				}
			break;

			case ModRegRMFormat::SegReg: {
				source_ = memreg;

				constexpr Source seg_table[4] = {
					Source::ES,	Source::CS,
					Source::SS,	Source::DS,
				};

				if(reg & 4) {
					undefined();
				}

				destination_ = seg_table[reg];
			} break;

			case ModRegRMFormat::MemRegROL_to_SAR:
				destination_ = memreg;

				switch(reg) {
					default: 	undefined();

					case 0: 	operation_ = Operation::ROL;	break;
					case 2: 	operation_ = Operation::ROR;	break;
					case 3: 	operation_ = Operation::RCL;	break;
					case 4: 	operation_ = Operation::RCR;	break;
					case 5: 	operation_ = Operation::SAL;	break;
					case 6: 	operation_ = Operation::SHR;	break;
					case 7: 	operation_ = Operation::SAR;	break;
				}
			break;

			case ModRegRMFormat::MemRegINC_DEC:
				source_ = destination_ = memreg;

				switch(reg) {
					default: 	undefined();

					case 0:		operation_ = Operation::INC;	break;
					case 1:		operation_ = Operation::DEC;	break;
				}
			break;

			case ModRegRMFormat::MemRegINC_to_PUSH:
				source_ = destination_ = memreg;

				switch(reg) {
					default: 	undefined();

					case 0:		operation_ = Operation::INC;	break;
					case 1:		operation_ = Operation::DEC;	break;
					case 2:		operation_ = Operation::CALLN;	break;
					case 3:
						operation_ = Operation::CALLF;
						operand_size_ = 4;
						source_ = Source::Immediate;
					break;
					case 4:		operation_ = Operation::JMPN;	break;
					case 5:
						operation_ = Operation::JMPF;
						operand_size_ = 4;
						source_ = Source::Immediate;
					break;
					case 6:	operation_ = Operation::PUSH;		break;
				}
			break;

			case ModRegRMFormat::MemRegPOP:
				source_ = destination_ = memreg;

				if(reg != 0) {
					undefined();
				}
			break;

			case ModRegRMFormat::MemRegMOV:
				source_ = Source::Immediate;
				destination_ = memreg;
				operand_size_ = operation_size_;
			break;

			case ModRegRMFormat::MemRegADD_to_CMP:
				destination_ = memreg;
				operand_size_ = operation_size_;

				switch(reg) {
					default:	operation_ = Operation::ADD;	break;
					case 1:		operation_ = Operation::OR;		break;
					case 2:		operation_ = Operation::ADC;	break;
					case 3:		operation_ = Operation::SBB;	break;
					case 4:		operation_ = Operation::AND;	break;
					case 5:		operation_ = Operation::SUB;	break;
					case 6:		operation_ = Operation::XOR;	break;
					case 7:		operation_ = Operation::CMP;	break;
				}
			break;

			case ModRegRMFormat::MemRegADC_to_CMP:
				destination_ = memreg;
				source_ = Source::Immediate;
				operand_size_ = 1;	// ... and always 1; it'll be sign extended if
									// the operation requires it.

				switch(reg) {
					default: undefined();

					case 0: 	operation_ = Operation::ADD;	break;
					case 2: 	operation_ = Operation::ADC;	break;
					case 3: 	operation_ = Operation::SBB;	break;
					case 5: 	operation_ = Operation::SUB;	break;
					case 7: 	operation_ = Operation::CMP;	break;
				}
			break;

			case ModRegRMFormat::MemRegSLDT_to_VERW:
				destination_ = source_ = memreg;

				switch(reg) {
					default: undefined();

					case 0: 	operation_ = Operation::SLDT;	break;
					case 1: 	operation_ = Operation::STR;	break;
					case 2: 	operation_ = Operation::LLDT;	break;
					case 3: 	operation_ = Operation::LTR;	break;
					case 4: 	operation_ = Operation::VERR;	break;
					case 5: 	operation_ = Operation::VERW;	break;
				}
			break;

			case ModRegRMFormat::MemRegSGDT_to_LMSW:
				destination_ = source_ = memreg;

				switch(reg) {
					default: undefined();

					case 0: 	operation_ = Operation::SGDT;	break;
					case 2: 	operation_ = Operation::LGDT;	break;
					case 4: 	operation_ = Operation::SMSW;	break;
					case 6: 	operation_ = Operation::LMSW;	break;
				}
			break;

			default: assert(false);
		}

		phase_ = (displacement_size_ + operand_size_) ? Phase::DisplacementOrOperand : Phase::ReadyToPost;
	}

	// MARK: - ScaleIndexBase

	if(phase_ == Phase::ScaleIndexBase && source != end) {
		sib_ = *source;
		++source;
		++consumed_;
	}

	// MARK: - Displacement and operand.

	if(phase_ == Phase::DisplacementOrOperand && source != end) {
		const int required_bytes = displacement_size_ + operand_size_;

		const int outstanding_bytes = required_bytes - operand_bytes_;
		const int bytes_to_consume = std::min(int(end - source), outstanding_bytes);

		// TODO: I can surely do better than this?
		for(int c = 0; c < bytes_to_consume; c++) {
			inward_data_ = (inward_data_ >> 8) | (uint64_t(source[0]) << 56);
			++source;
		}

		consumed_ += bytes_to_consume;
		operand_bytes_ += bytes_to_consume;

		if(bytes_to_consume == outstanding_bytes) {
			phase_ = Phase::ReadyToPost;

			switch(operand_size_) {
				default:	operand_ = 0;										break;
				case 1:
					operand_ = inward_data_ >> 56; inward_data_ <<= 8;

					// Sign extend if a single byte operand is feeding a two-byte instruction.
					if(operation_size_ == 2 && operation_ != Operation::IN && operation_ != Operation::OUT) {
						operand_ |= (operand_ & 0x80) ? 0xff00 : 0x0000;
					}
				break;
				case 4:		displacement_size_ = 2;								[[fallthrough]];
				case 2:		operand_ = inward_data_ >> 48; inward_data_ <<= 16;	break;
				break;
			}
			switch(displacement_size_) {
				default:	displacement_ = 0;									break;
				case 1:		displacement_ = int8_t(inward_data_ >> 56);			break;
				case 2:		displacement_ = int16_t(inward_data_ >> 48);		break;
			}
		} else {
			// Provide a genuine measure of further bytes required.
			return std::make_pair(-(outstanding_bytes - bytes_to_consume), InstructionT());
		}
	}

	// MARK: - Check for completion.

	if(phase_ == Phase::ReadyToPost) {
		const auto result = std::make_pair(
			consumed_,
			InstructionT(
				operation_,
				source_,
				destination_,
				sib_,
				lock_,
				address_size_,
				segment_override_,
				repetition_,
				Size(operation_size_),
				displacement_,
				operand_)
		);
		reset_parsing();
		return result;
	}

	// i.e. not done yet.
	return std::make_pair(0, InstructionT());
}

// Ensure all possible decoders are built.
template class InstructionSet::x86::Decoder<InstructionSet::x86::Model::i8086>;
template class InstructionSet::x86::Decoder<InstructionSet::x86::Model::i80186>;
template class InstructionSet::x86::Decoder<InstructionSet::x86::Model::i80286>;
template class InstructionSet::x86::Decoder<InstructionSet::x86::Model::i80386>;
