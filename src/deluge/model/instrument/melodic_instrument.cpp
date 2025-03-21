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

#include "model/instrument/melodic_instrument.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/ui/keyboard/keyboard_screen.h"
#include "gui/views/automation_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_follow.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/midi_instrument.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "storage/storage_manager.h"
#include <cstring>

bool MelodicInstrument::writeMelodicInstrumentAttributesToFile(Serializer& writer, Clip* clipForSavingOutputOnly,
                                                               Song* song) {
	Instrument::writeDataToFile(writer, clipForSavingOutputOnly, song);
	if (!clipForSavingOutputOnly) {

		// Annoyingly, I used one-off tag names here, rather than it conforming to what the LearnedMIDI class now uses.
		// Channel gets written here as an attribute. Device, gets written below, as a tag.
		if (midiInput.containsSomething()) {
			if (midiInput.isForMPEZone()) {
				char const* zoneText = (midiInput.channelOrZone == MIDI_CHANNEL_MPE_LOWER_ZONE) ? "lower" : "upper";
				writer.writeAttribute("inputMPEZone", zoneText);
			}
			else {
				writer.writeAttribute("inputMidiChannel", midiInput.channelOrZone);
			}
		}
	}

	return false;
}

void MelodicInstrument::writeMelodicInstrumentTagsToFile(Serializer& writer, Clip* clipForSavingOutputOnly,
                                                         Song* song) {

	if (!clipForSavingOutputOnly) {
		// Annoyingly, I used one-off tag names here, rather than it conforming to what the LearnedMIDI class now uses.
		if (midiInput.containsSomething()) {
			// Device gets written here as a tag. Channel got written above, as an attribute.
			if (midiInput.device) {
				midiInput.device->writeReferenceToFile(writer, "inputMidiDevice");
			}
		}
	}
}

bool MelodicInstrument::readTagFromFile(Deserializer& reader, char const* tagName) {

	// Annoyingly, I used one-off tag names here, rather than it conforming to what the LearnedMIDI class now uses.
	if (!strcmp(tagName, "inputMidiChannel")) {
		midiInput.channelOrZone = reader.readTagOrAttributeValueInt();
		reader.exitTag();
	}
	else if (!strcmp(tagName, "inputMPEZone")) {
		midiInput.readMPEZone(reader);
		reader.exitTag();
	}
	else if (!strcmp(tagName, "inputMidiDevice")) {
		midiInput.device = MIDIDeviceManager::readDeviceReferenceFromFile(reader);
		reader.exitTag();
	}
	else if (Instrument::readTagFromFile(reader, tagName)) {}
	else {
		return false;
	}

	return true;
}

void MelodicInstrument::receivedNote(ModelStackWithTimelineCounter* modelStack, MIDIDevice* fromDevice, bool on,
                                     int32_t midiChannel, MIDIMatchType match, int32_t note, int32_t velocity,
                                     bool shouldRecordNotes, bool* doingMidiThru) {
	int16_t const* mpeValues = zeroMPEValues;
	int16_t const* mpeValuesOrNull = NULL;
	int32_t highlightNoteValue = -1;
	switch (match) {
	case MIDIMatchType::NO_MATCH:
		return;
	case MIDIMatchType::MPE_MASTER:
	case MIDIMatchType::MPE_MEMBER:
		mpeValues = mpeValuesOrNull = fromDevice->defaultInputMPEValuesPerMIDIChannel[midiChannel];
		// no break
	case MIDIMatchType::CHANNEL:
		// -1 means no change
		InstrumentClip* instrumentClip = (InstrumentClip*)activeClip;

		ModelStackWithNoteRow* modelStackWithNoteRow =
		    instrumentClip ? instrumentClip->getNoteRowForYNote(note, modelStack) : modelStack->addNoteRow(0, NULL);

		NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();

		// Note-on
		if (on) {
			if (runtimeFeatureSettings.get(RuntimeFeatureSettingType::HighlightIncomingNotes)
			        == RuntimeFeatureStateToggle::On
			    && instrumentClip == getCurrentInstrumentClip()) {
				highlightNoteValue = velocity;
			}

			// MPE stuff - if editing note, we need to record the initial values which might have been sent before this
			// note-on.
			instrumentClipView.reportMPEInitialValuesForNoteEditing(
			    modelStackWithNoteRow,
			    mpeValues); // Hmm, should we really be going in here even when it's not MPE input?

			// NoteRow must not already be sounding a note
			if (!noteRow || !noteRow->soundingStatus) {

				if (instrumentClip) {

					// If we wanna record...
					if (shouldRecordNotes) {

						bool forcePos0 = false;

						// Special case - if recording session to arrangement, then yes we do want to record to the Clip
						// always (even if not designated as "active")
						if (playbackHandler.recording == RecordingMode::ARRANGEMENT
						    && instrumentClip->isArrangementOnlyClip()) {
							goto doRecord;

							// If count-in is on, we only got here if it's very nearly finished
						}
						else if (currentUIMode == UI_MODE_RECORD_COUNT_IN) {
recordingEarly:
							earlyNotes.insertElementIfNonePresent(
							    note, velocity, instrumentClip->allowNoteTails(modelStackWithNoteRow));
							goto justAuditionNote;
						}

						// And another special case - if there's a linear recording beginning really soon,
						// and activeClip is not linearly recording (and maybe not even active)...
						else if (currentPlaybackMode == &session && session.launchEventAtSwungTickCount
						         && !instrumentClip->getCurrentlyRecordingLinearly()) {
							int32_t ticksTilLaunch =
							    session.launchEventAtSwungTickCount - playbackHandler.getActualSwungTickCount();
							int32_t samplesTilLaunch = ticksTilLaunch * playbackHandler.getTimePerInternalTick();

							if (samplesTilLaunch <= kLinearRecordingEarlyFirstNoteAllowance) {
								Clip* clipAboutToRecord =
								    currentSong->getClipWithOutputAboutToBeginLinearRecording(this);
								if (clipAboutToRecord) {
									goto recordingEarly;
								}
							}
						}

						// Ok, special case checking is all done - do the normal thing.

						// If Clip is active, nice and easy - we know we can record to it
						if (currentSong->isClipActive(instrumentClip)) {
doRecord:
							instrumentClip->possiblyCloneForArrangementRecording(
							    modelStack); // Will have to re-get modelStackWithNoteRow after this call
							instrumentClip =
							    (InstrumentClip*)
							        modelStack->getTimelineCounter(); // Re-get it, cos it might have changed

							Action* action = actionLogger.getNewAction(ActionType::RECORD, ActionAddition::ALLOWED);

							bool scaleAltered = false;

							modelStackWithNoteRow = instrumentClip->getOrCreateNoteRowForYNote(
							    note, modelStack, action,
							    &scaleAltered); // Have to re-get this anyway since called
							                    // possiblyCloneForArrangementRecording(), above.
							noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
							if (noteRow) {
								// midichannel is not used by instrument clip
								instrumentClip->recordNoteOn(modelStackWithNoteRow, velocity, forcePos0,
								                             mpeValuesOrNull, midiChannel);
								if (getRootUI()) {
									getRootUI()->noteRowChanged(instrumentClip, noteRow);
								}
							}

							// If this caused the scale to change, update scroll
							if (action && scaleAltered) {
								action->updateYScrollClipViewAfter();
							}
						}
					}
				}
justAuditionNote:
				/*
				int16_t tempExpressionParams[NUM_MPE_SOURCES];
				// If it was not MPE, then get the pitch bend and channel pressure values from the ParamManager, so e.g.
				the synth can make the correct sound on
				// the note-on it's going to do now.
				if (mpeValues == zeroMPEValues) {
				    ParamManager* paramManager = getParamManager(modelStackWithNoteRow->song);
				    MPEParamSet* expressionParams = paramManager->getMPEParamSet();
				    if (expressionParams) {
				        for (int32_t m = 0; m < NUM_MPE_SOURCES; m++) {
				            tempExpressionParams[m] = expressionParams->params[m].getCurrentValue() >> 16;
				        }
				        mpeValues = tempExpressionParams;
				    }
				}
				*/
				beginAuditioningForNote(modelStack->toWithSong(), // Safe, cos we won't reference this again
				                        note, velocity, mpeValues, midiChannel);
			}
		}

		// Note-off
		else {
			if (runtimeFeatureSettings.get(RuntimeFeatureSettingType::HighlightIncomingNotes)
			        == RuntimeFeatureStateToggle::On
			    && instrumentClip == getCurrentInstrumentClip()) {
				highlightNoteValue = 0;
			}
			// NoteRow must already be auditioning
			if (notesAuditioned.searchExact(note) != -1) {

				if (noteRow) {
					// If we get here, we know there is a Clip
					if (shouldRecordNotes
					    && ((playbackHandler.recording == RecordingMode::ARRANGEMENT
					         && instrumentClip->isArrangementOnlyClip())
					        || currentSong->isClipActive(instrumentClip))) {

						if (playbackHandler.recording == RecordingMode::ARRANGEMENT
						    && !instrumentClip->isArrangementOnlyClip()) {}
						else {
							instrumentClip->recordNoteOff(modelStackWithNoteRow, velocity);
							if (getRootUI()) {
								getRootUI()->noteRowChanged(instrumentClip, noteRow);
							}
						}
					}

					instrumentClipView.reportNoteOffForMPEEditing(modelStackWithNoteRow);
				}
			}

			if (noteRow) {
				// MPE-controlled params are a bit special in that we can see (via this note-off) when the user has
				// removed their finger and won't be sending more values. So, let's unlatch those params now.
				ExpressionParamSet* mpeParams = noteRow->paramManager.getExpressionParamSet();
				if (mpeParams) {
					mpeParams->cancelAllOverriding();
				}
			}

			// We want to make sure we sent the note-off even if it didn't think auditioning was happening. This is to
			// stop a stuck note if MIDI thru was on and they're releasing the note while still holding learn to learn
			// that input to a MIDIInstrument (with external synth attached)
			endAuditioningForNote(modelStack->toWithSong(), // Safe, cos we won't reference this again
			                      note, velocity);
		}
	} // end match switch

	if (highlightNoteValue != -1) {
		keyboardScreen.highlightedNotes[note] = highlightNoteValue;
		keyboardScreen.requestRendering();
	}
}

void MelodicInstrument::offerReceivedNote(ModelStackWithTimelineCounter* modelStack, MIDIDevice* fromDevice, bool on,
                                          int32_t midiChannel, int32_t note, int32_t velocity, bool shouldRecordNotes,
                                          bool* doingMidiThru) {
	MIDIMatchType match = midiInput.checkMatch(fromDevice, midiChannel);
	auto* instrumentClip = static_cast<InstrumentClip*>(activeClip);

	if (match != MIDIMatchType::NO_MATCH) {
		receivedNote(modelStack, fromDevice, on, midiChannel, match, note, velocity, shouldRecordNotes, doingMidiThru);
	}
	// In case Norns layout is active show
	// this ignores input differentiation, but since midi learn doesn't work for norns grid
	// you can't set a device
	// norns midigrid mod sends deluge midi note_on messages on channel 16 to update pad brightness
	else if (instrumentClip->keyboardState.currentLayout == KeyboardLayoutType::KeyboardLayoutTypeNorns
	         && instrumentClip->onKeyboardScreen && instrumentClip->output
	         && instrumentClip->output->type == OutputType::MIDI_OUT
	         && ((MIDIInstrument*)instrumentClip->output)->channel == midiChannel) {
		keyboardScreen.nornsNotes[note] = on ? velocity : 0;
		keyboardScreen.requestRendering();
	}
}

void MelodicInstrument::offerReceivedPitchBend(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                               MIDIDevice* fromDevice, uint8_t channel, uint8_t data1, uint8_t data2,
                                               bool* doingMidiThru) {
	MIDIMatchType match = midiInput.checkMatch(fromDevice, channel);
	if (match != MIDIMatchType::NO_MATCH) {
		receivedPitchBend(modelStackWithTimelineCounter, fromDevice, match, channel, data1, data2, doingMidiThru);
	}
}

void MelodicInstrument::receivedPitchBend(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                          MIDIDevice* fromDevice, MIDIMatchType match, uint8_t channel, uint8_t data1,
                                          uint8_t data2, bool* doingMidiThru) {
	int32_t newValue;
	switch (match) {

	case MIDIMatchType::NO_MATCH:
		return;
	case MIDIMatchType::MPE_MEMBER:
		// each of these are 7 bit values but we need them to represent the range +-2^31
		newValue = (int32_t)(((uint32_t)data1 | ((uint32_t)data2 << 7)) - 8192) << 18;
		// Unlike for whole-Instrument pitch bend, this per-note kind is a modulation *source*, not the "preset" value
		// for the parameter!
		polyphonicExpressionEventPossiblyToRecord(modelStackWithTimelineCounter, newValue, X_PITCH_BEND, channel,
		                                          MIDICharacteristic::CHANNEL);
		break;
	case MIDIMatchType::MPE_MASTER:
	case MIDIMatchType::CHANNEL:
		// If it's a MIDIInstrtument...
		if (type == OutputType::MIDI_OUT) {
			// .. and it's outputting on the same channel as this MIDI message came in, don't do MIDI thru!
			if (doingMidiThru && ((MIDIInstrument*)this)->channel == channel) {
				*doingMidiThru = false;
			}
		}

		// Still send the pitch-bend even if the Output is muted. MidiInstruments will check for and block this
		// themselves

		newValue = (int32_t)(((uint32_t)data1 | ((uint32_t)data2 << 7)) - 8192) << 18;
		processParamFromInputMIDIChannel(CC_NUMBER_PITCH_BEND, newValue, modelStackWithTimelineCounter);
		break;
	}
}
void MelodicInstrument::offerReceivedCC(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                        MIDIDevice* fromDevice, uint8_t channel, uint8_t ccNumber, uint8_t value,
                                        bool* doingMidiThru) {
	MIDIMatchType match = midiInput.checkMatch(fromDevice, channel);
	if (match != MIDIMatchType::NO_MATCH) {
		receivedCC(modelStackWithTimelineCounter, fromDevice, match, channel, ccNumber, value, doingMidiThru);
		namespace params = deluge::modulation::params;
		if (fromDevice == &MIDIDeviceManager::loopbackMidi) {
			midiFollow.handleReceivedCC(*modelStackWithTimelineCounter, activeClip, ccNumber, value);
		}
	}
}
void MelodicInstrument::receivedCC(ModelStackWithTimelineCounter* modelStackWithTimelineCounter, MIDIDevice* fromDevice,
                                   MIDIMatchType match, uint8_t channel, uint8_t ccNumber, uint8_t value,
                                   bool* doingMidiThru) {
	// ideally this would be configurable
	int yCC = 1;
	int32_t value32 = 0;
	switch (match) {

	case MIDIMatchType::NO_MATCH:
		return;
	case MIDIMatchType::MPE_MEMBER:
		if (ccNumber == 74) { // All other CCs are not supposed to be used for Member Channels, for anything.
			int32_t value32 = (value - 64) << 25;
			polyphonicExpressionEventPossiblyToRecord(modelStackWithTimelineCounter, value32, Y_SLIDE_TIMBRE, channel,
			                                          MIDICharacteristic::CHANNEL);

			possiblyRefreshAutomationEditorGrid(ccNumber);

			return;
		}
	case MIDIMatchType::MPE_MASTER:
		[[fallthrough]];
	case MIDIMatchType::CHANNEL:
		// If it's a MIDI Clip...
		if (type == OutputType::MIDI_OUT) {
			// .. and it's outputting on the same channel as this MIDI message came in, don't do MIDI thru!
			if (doingMidiThru && ((MIDIInstrument*)this)->channel == channel) {
				*doingMidiThru = false;
			}
		}
		if (ccNumber == yCC) {
			// this is the same range as mpe Y axis but unipolar
			value32 = (value) << 24;
			processParamFromInputMIDIChannel(CC_NUMBER_Y_AXIS, value32, modelStackWithTimelineCounter);
			// Don't also pass to ccReveived since it will now be handled by output mono expression in midi
			// clips instead
			return;
		}

		// Still send the cc even if the Output is muted. MidiInstruments will check for and block this
		// themselves
		ccReceivedFromInputMIDIChannel(ccNumber, value, modelStackWithTimelineCounter);

		possiblyRefreshAutomationEditorGrid(ccNumber);
	}
}

void MelodicInstrument::possiblyRefreshAutomationEditorGrid(int32_t ccNumber) {
	// if you're in automation midi clip view and editing the same CC that was just updated
	// by a learned midi knob, then re-render the pads on the automation editor grid
	if (type == OutputType::MIDI_OUT) {
		if (getRootUI() == &automationView) {
			if (activeClip->lastSelectedParamID == ccNumber) {
				uiNeedsRendering(&automationView);
			}
		}
	}
}

void MelodicInstrument::offerReceivedAftertouch(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                                MIDIDevice* fromDevice, int32_t channel, int32_t value,
                                                int32_t noteCode, bool* doingMidiThru) {
	MIDIMatchType match = midiInput.checkMatch(fromDevice, channel);
	if (match != MIDIMatchType::NO_MATCH) {
		receivedAftertouch(modelStackWithTimelineCounter, fromDevice, match, channel, value, noteCode, doingMidiThru);
	}
}

// noteCode -1 means channel-wide, including for MPE input (which then means it could still then just apply to
// one note).
void MelodicInstrument::receivedAftertouch(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                           MIDIDevice* fromDevice, MIDIMatchType match, int32_t channel, int32_t value,
                                           int32_t noteCode, bool* doingMidiThru) {
	int32_t valueBig = (int32_t)value << 24;
	switch (match) {

	case MIDIMatchType::NO_MATCH:
		return;
	case MIDIMatchType::MPE_MEMBER:
		polyphonicExpressionEventPossiblyToRecord(modelStackWithTimelineCounter, valueBig, Z_PRESSURE, channel,
		                                          MIDICharacteristic::CHANNEL);
		break;
	case MIDIMatchType::MPE_MASTER:
	case MIDIMatchType::CHANNEL:
		// If it's a MIDI Clip...
		if (type == OutputType::MIDI_OUT) {
			// .. and it's outputting on the same channel as this MIDI message came in, don't do MIDI thru!
			if (doingMidiThru && ((MIDIInstrument*)this)->channel == channel) {
				*doingMidiThru = false;
			}
		}

		// Still send the aftertouch even if the Output is muted. MidiInstruments will check for and block this
		// themselves MPE should never send poly aftertouch but we might as well handle it anyway Polyphonic
		// aftertouch gets processed along with MPE
		if (noteCode != -1) {
			polyphonicExpressionEventPossiblyToRecord(modelStackWithTimelineCounter, valueBig, Z_PRESSURE, noteCode,
			                                          MIDICharacteristic::NOTE);
			// We wouldn't be here if this was MPE input, so we know this incoming polyphonic aftertouch message
			// is allowed
		}

		// Or, channel pressure
		else {
			processParamFromInputMIDIChannel(CC_NUMBER_AFTERTOUCH, valueBig, modelStackWithTimelineCounter);
		}
	}
}

void MelodicInstrument::offerBendRangeUpdate(ModelStack* modelStack, MIDIDevice* device, int32_t channelOrZone,
                                             int32_t whichBendRange, int32_t bendSemitones) {

	if (midiInput.equalsChannelOrZone(device, channelOrZone)) {

		ParamManager* paramManager = getParamManager(modelStack->song);
		if (paramManager) { // It could be NULL! - for a CVInstrument.
			ExpressionParamSet* expressionParams = paramManager->getOrCreateExpressionParamSet();
			if (expressionParams) {
				// If existing automation, don't do it.
				if (activeClip) {
					if (whichBendRange == BEND_RANGE_MAIN) {
						if (expressionParams->params[0].isAutomated()) {
							return;
						}
					}
					else { // BEND_RANGE_FINGER_LEVEL
						if (((InstrumentClip*)activeClip)->hasAnyPitchExpressionAutomationOnNoteRows()) {
							return;
						}
					}
				}
				expressionParams->bendRanges[whichBendRange] = bendSemitones;
			}
		}
	}
}

bool MelodicInstrument::setActiveClip(ModelStackWithTimelineCounter* modelStack, PgmChangeSend maySendMIDIPGMs) {

	earlyNotes.empty();

	return Instrument::setActiveClip(modelStack, maySendMIDIPGMs);
}

bool MelodicInstrument::isNoteRowStillAuditioningAsLinearRecordingEnded(NoteRow* noteRow) {
	return (notesAuditioned.searchExact(noteRow->y) != -1) && (earlyNotes.searchExact(noteRow->y) == -1);
}

void MelodicInstrument::stopAnyAuditioning(ModelStack* modelStack) {

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addTimelineCounter(activeClip)
	        ->addOtherTwoThingsButNoNoteRow(toModControllable(), getParamManager(modelStack->song));

	for (int32_t i = 0; i < notesAuditioned.getNumElements(); i++) {
		EarlyNote* note = (EarlyNote*)notesAuditioned.getElementAddress(i);
		sendNote(modelStackWithThreeMainThings, false, note->note, NULL);
	}

	notesAuditioned.empty();
	earlyNotes.empty(); // This is fine, though in a perfect world we'd prefer to just mark the notes as no
	                    // longer active
	if (activeClip) {
		activeClip->expectEvent(); // Because the absence of auditioning here means sequenced notes may play
	}
}

bool MelodicInstrument::isNoteAuditioning(int32_t noteCode) {
	return (notesAuditioned.searchExact(noteCode) != -1);
}

void MelodicInstrument::beginAuditioningForNote(ModelStack* modelStack, int32_t note, int32_t velocity,
                                                int16_t const* mpeValues, int32_t fromMIDIChannel,
                                                uint32_t sampleSyncLength) {

	ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addTimelineCounter(activeClip)->addNoteRow(0, NULL);
	if (!activeClip || ((InstrumentClip*)activeClip)->allowNoteTails(modelStackWithNoteRow)) {
		notesAuditioned.insertElementIfNonePresent(note, velocity);
	}

	ParamManager* paramManager = getParamManager(modelStackWithNoteRow->song);
	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStackWithNoteRow->addOtherTwoThings(toModControllable(), paramManager);

	sendNote(modelStackWithThreeMainThings, true, note, mpeValues, fromMIDIChannel, velocity, sampleSyncLength);
}

void MelodicInstrument::endAuditioningForNote(ModelStack* modelStack, int32_t note, int32_t velocity) {
	notesAuditioned.deleteAtKey(note);
	earlyNotes.noteNoLongerActive(note);
	if (activeClip) {
		activeClip->expectEvent(); // Because the absence of auditioning here means sequenced notes may play
	}

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addTimelineCounter(activeClip)
	        ->addOtherTwoThingsButNoNoteRow(toModControllable(), getParamManager(modelStack->song));

	sendNote(modelStackWithThreeMainThings, false, note, NULL, MIDI_CHANNEL_NONE, velocity);
}

bool MelodicInstrument::isAnyAuditioningHappening() {
	return notesAuditioned.getNumElements();
}

// Virtual function, gets overridden.
ModelStackWithAutoParam*
MelodicInstrument::getParamToControlFromInputMIDIChannel(int32_t cc, ModelStackWithThreeMainThings* modelStack) {

	modelStack->paramManager->ensureExpressionParamSetExists();
	ParamCollectionSummary* summary = modelStack->paramManager->getExpressionParamSetSummary();

	ExpressionParamSet* mpeParams = (ExpressionParamSet*)summary->paramCollection;
	if (!mpeParams) {
		return modelStack->addParam(NULL, NULL, 0, NULL); // Crude way of saying "none".
	}

	int32_t paramId;

	switch (cc) {
	case CC_NUMBER_PITCH_BEND:
		paramId = 0;
		break;

	case CC_NUMBER_Y_AXIS:
		paramId = 1;
		break;

	case CC_NUMBER_AFTERTOUCH:
		paramId = 2;
		break;

	default:
		__builtin_unreachable();
	}

	return modelStack->addParam(mpeParams, summary, paramId, &mpeParams->params[paramId]);
}

// Big part of this function is that it can decide to call possiblyCloneForArrangementRecording().
void MelodicInstrument::processParamFromInputMIDIChannel(int32_t cc, int32_t newValue,
                                                         ModelStackWithTimelineCounter* modelStack) {

	int32_t modPos = 0;
	int32_t modLength = 0;

	if (modelStack->timelineCounterIsSet()) {
		modelStack->getTimelineCounter()->possiblyCloneForArrangementRecording(modelStack);

		// Only if this exact TimelineCounter is having automation step-edited, we can set the value for just a
		// region.
		if (view.modLength
		    && modelStack->getTimelineCounter() == view.activeModControllableModelStack.getTimelineCounterAllowNull()) {
			modPos = view.modPos;
			modLength = view.modLength;
		}
	}

	ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(0, NULL);

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStackWithNoteRow->addOtherTwoThings(toModControllable(), getParamManager(modelStack->song));

	ModelStackWithAutoParam* modelStackWithParam =
	    getParamToControlFromInputMIDIChannel(cc, modelStackWithThreeMainThings);

	if (modelStackWithParam->autoParam) {
		modelStackWithParam->autoParam->setValuePossiblyForRegion(
		    newValue, modelStackWithParam, modPos, modLength,
		    false); // Don't delete nodes in linear run, cos this might need to be outputted as MIDI again
	}
}

ArpeggiatorSettings* MelodicInstrument::getArpSettings(InstrumentClip* clip) {
	if (clip) {
		return &clip->arpSettings;
	}
	else if (activeClip) {
		return &((InstrumentClip*)activeClip)->arpSettings;
	}
	else {
		return NULL;
	}
}

bool expressionValueChangesMustBeDoneSmoothly = false; // Wee bit of a workaround

// Ok this is similar to processParamFromInputMIDIChannel(), above, but for MPE. It's different because one
// input message might have multiple AutoParams it applies to (i.e. because the member channel might have
// multiple notes / NoteRows). And also because the AutoParam is allowed to not exist at all - e.g. if there's
// no NoteRow for the note
// - but we still want to cause a sound change in response to the message.
void MelodicInstrument::polyphonicExpressionEventPossiblyToRecord(ModelStackWithTimelineCounter* modelStack,
                                                                  int32_t newValue, int32_t whichExpressionDimension,
                                                                  int32_t channelOrNoteNumber,
                                                                  MIDICharacteristic whichCharacteristic) {
	expressionValueChangesMustBeDoneSmoothly = true;

	// If recording, we send the new value to the AutoParam, which will also sound that change right now.
	if (modelStack->timelineCounterIsSet()) { // && playbackHandler.isEitherClockActive() &&
		                                      // playbackHandler.recording) {
		modelStack->getTimelineCounter()->possiblyCloneForArrangementRecording(modelStack);

		for (int32_t n = 0; n < arpeggiator.notes.getNumElements(); n++) {
			ArpNote* arpNote = (ArpNote*)arpeggiator.notes.getElementAddress(n);
			if (arpNote->inputCharacteristics[util::to_underlying(whichCharacteristic)]
			    == channelOrNoteNumber) { // If we're actually identifying by MIDICharacteristic::NOTE, we could
				                          // do a much faster search,
				// but let's not bother - that's only done when we're receiving MIDI polyphonic aftertouch
				// messages, and there's hardly much to search through.
				ModelStackWithNoteRow* modelStackWithNoteRow =
				    ((InstrumentClip*)modelStack->getTimelineCounter())
				        ->getNoteRowForYNote(
				            arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)],
				            modelStack); // No need to create - it should already exist if they're recording a
				                         // note here.
				NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
				if (noteRow) {
					bool success = noteRow->recordPolyphonicExpressionEvent(modelStackWithNoteRow, newValue,
					                                                        whichExpressionDimension, false);
					if (success) {
						continue;
					}
				}

				// If still here, that didn't work, so just send it without recording.
				polyphonicExpressionEventOnChannelOrNote(
				    newValue, whichExpressionDimension,
				    arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)],
				    MIDICharacteristic::NOTE);
			}
		}
	}

	// Or if not recording, just sound the change ourselves here (as opposed to the AutoParam doing it).
	else {
		polyphonicExpressionEventOnChannelOrNote(newValue, whichExpressionDimension, channelOrNoteNumber,
		                                         whichCharacteristic);
	}

	expressionValueChangesMustBeDoneSmoothly = false;
}

ModelStackWithAutoParam* MelodicInstrument::getModelStackWithParam(ModelStackWithTimelineCounter* modelStack,
                                                                   Clip* clip, int32_t paramID,
                                                                   deluge::modulation::params::Kind paramKind,
                                                                   bool affectEntire, bool useMenuStack) {
	ModelStackWithAutoParam* modelStackWithParam = nullptr;

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addOtherTwoThingsButNoNoteRow(toModControllable(), &clip->paramManager);

	if (modelStackWithThreeMainThings) {
		if (paramKind == deluge::modulation::params::Kind::PATCHED) {
			modelStackWithParam = modelStackWithThreeMainThings->getPatchedAutoParamFromId(paramID);
		}

		else if (paramKind == deluge::modulation::params::Kind::UNPATCHED_SOUND) {
			modelStackWithParam = modelStackWithThreeMainThings->getUnpatchedAutoParamFromId(paramID);
		}

		else if (paramKind == deluge::modulation::params::Kind::PATCH_CABLE) {
			modelStackWithParam = modelStackWithThreeMainThings->getPatchCableAutoParamFromId(paramID);
		}
	}

	return modelStackWithParam;
}
