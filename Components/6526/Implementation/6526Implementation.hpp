//
//  6526Implementation.hpp
//  Clock Signal
//
//  Created by Thomas Harte on 18/07/2021.
//  Copyright © 2021 Thomas Harte. All rights reserved.
//

#ifndef _526Implementation_h
#define _526Implementation_h

#include <cassert>
#include <cstdio>

namespace MOS {
namespace MOS6526 {

template <typename BusHandlerT, Personality personality>
void MOS6526<BusHandlerT, personality>::write(int address, uint8_t value) {
	address &= 0xf;
	switch(address) {
		case 2: case 3:
			registers_.data_direction[address - 2] = value;
		break;

		default:
			printf("Unhandled 6526 write: %02x to %d\n", value, address);
			assert(false);
		break;
	}
}

template <typename BusHandlerT, Personality personality>
uint8_t MOS6526<BusHandlerT, personality>::read(int address) {
	address &= 0xf;
	switch(address) {
		case 2: case 3:
			return registers_.data_direction[address - 2];
		break;

		default:
			printf("Unhandled 6526 read from %d\n", address);
			assert(false);
		break;
	}
	return 0xff;
}

template <typename BusHandlerT, Personality personality>
void MOS6526<BusHandlerT, personality>::run_for(const HalfCycles half_cycles) {
	(void)half_cycles;
}

}
}

#endif /* _526Implementation_h */
