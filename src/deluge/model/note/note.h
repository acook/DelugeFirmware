/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>

#include "model/positionable.h"

class Note : public Positionable {
public:
	Note();

	inline void setLength(int32_t newLength) { length = newLength; }

	inline int32_t getLength() { return length; }

	inline void setVelocity(int32_t newVelocity) { velocity = newVelocity; }

	inline int32_t getVelocity() { return velocity; }

	inline void setLift(int32_t newLift) { lift = newLift; }

	inline int32_t getLift() { return lift; }

	inline void setProbability(int32_t newProbability) { probability = newProbability; }

	inline int32_t getProbability() { return probability; }

	//	void writeToFile();

	int32_t length;
	uint8_t velocity;
	uint8_t probability;
	uint8_t lift;

	// Understanding the probability field: the first bit says whether it's based on a previous one.
	// So take the rightmost 7 bits. If that's greater than kNumProbabilityValues (20),
};
