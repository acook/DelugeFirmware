/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/notes_state.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "hid/button.h"
#include "model/clip/instrument_clip_minder.h"
#include "model/instrument/instrument.h"

class ModelStack;
class Instrument;

namespace deluge::gui::ui::keyboard {

constexpr int32_t kMaxNumKeyboardPadPresses = 10;

class KeyboardScreen final : public RootUI, public InstrumentClipMinder {
public:
	KeyboardScreen();

	ActionResult padAction(int32_t x, int32_t y, int32_t velocity);
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine);
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine);
	ActionResult horizontalEncoderAction(int32_t offset);
	void selectEncoderAction(int8_t offset);

	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = false);
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]);

	void flashDefaultRootNote();
	void openedInBackground();
	void exitAuditionMode();

	uint8_t highlightedNotes[kHighestKeyboardNote] = {0};
	uint8_t nornsNotes[kHighestKeyboardNote] = {0};

	inline void requestRendering() { uiNeedsRendering(this, 0xFFFFFFFF, 0xFFFFFFFF); }

	// ui
	UIType getUIType() { return UIType::KEYBOARD_SCREEN; }

	void checkNewInstrument(Instrument* newInstrument);

private:
	bool opened();
	void focusRegained();
	void displayOrLanguageChanged() final;

	void evaluateActiveNotes();
	void updateActiveNotes();

	void noteOff(ModelStack& modelStack, Instrument& activeInstrument, bool clipIsActiveOnInstrument, int32_t note);

	ClipMinder* toClipMinder() { return this; }
	void setLedStates();
	void graphicsRoutine();
	bool getAffectEntire();

	void unscrolledPadAudition(int32_t velocity, int32_t note, bool shiftButtonDown);

	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override {
		InstrumentClipMinder::renderOLED(canvas);
	}

	void selectLayout(int8_t offset);
	void enterScaleMode(int32_t selectedRootNote = kDefaultCalculateRootNote);
	void exitScaleMode();
	void drawNoteCode(int32_t noteCode);

	PressedPad pressedPads[kMaxNumKeyboardPadPresses];
	NotesState lastNotesState;
	NotesState currentNotesState;

	bool keyboardButtonActive = false;
	bool keyboardButtonUsed = false;
};
}; // namespace deluge::gui::ui::keyboard

// TODO: should get moved into namespace once project namespacing is complete
extern deluge::gui::ui::keyboard::KeyboardScreen keyboardScreen;
