/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#include "gui/menu_item/selection.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::sample {
template <size_t n>
class Selection : public menu_item::Selection {
public:
	using menu_item::Selection::Selection;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		if (sound == nullptr) {
			return true; // For AudioClips
		}

		Source* source = &sound->sources[whichThing];
		return (sound->getSynthMode() == SynthMode::SUBTRACTIVE && source->oscType == OscType::SAMPLE
		        && source->hasAtLeastOneAudioFileLoaded());
	}
};
} // namespace deluge::gui::menu_item::sample
