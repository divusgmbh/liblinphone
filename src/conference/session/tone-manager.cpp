/*
 * Copyright (c) 2010-2019 Belledonne Communications SARL.
 *
 * This file is part of Liblinphone.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tone-manager.h"
#include "linphone/utils/utils.h"
#include "logger/logger.h"
#include "call/call.h"
#include "conference/session/media-session.h"
#include "conference/session/media-session-p.h"
#include "conference/params/call-session-params-p.h"
#include "linphone/utils/general.h"
#include "core/core-p.h"
#include "conference_private.h"

#include <bctoolbox/defs.h>

LINPHONE_BEGIN_NAMESPACE

ToneManager::ToneManager(std::shared_ptr<Core> core) : CoreAccessor(core) {
    lInfo() << "[ToneManager] create ToneManager()";
	mStats = new LinphoneCoreToneManagerStats;
	*mStats = {0, 0, 0, 0, 0, 0, 0};
}

ToneManager::~ToneManager() {
    lInfo() << "[ToneManager] destroy ToneManager()";
	delete mStats;
}

std::string ToneManager::stateToString(ToneManager::State state) {
	switch(state) {
		case State::None:
			return "None";
		case State::Call:
			return "Call";
		case State::Ringback:
			return "Ringback";
		case State::Ringtone:
			return "Ringtone";
		case State::Tone:
			return "Tone";
		default:
			return "";
	}
}

void ToneManager::printDebugInfo(const std::shared_ptr<CallSession> &session) {
	auto callState = session->getState();
    auto toneState = getState(session);
	lInfo() << "[ToneManager] [" << session << "] state changed : [" << stateToString(toneState)  << ", " << Utils::toString(callState) << "]";
}

// ---------------------------------------------------
// public entrypoints for tones
// ---------------------------------------------------

void ToneManager::startRingbackTone(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__;
	printDebugInfo(session);
	if (getState(session) == State::Ringback) {
		return;
	}

	setState(session, State::Ringback);
	mStats->number_of_startRingbackTone++;

	
	if (session->getParams()->getPrivate()->getInConference()){
		lInfo() << "Skip ring back tone, call is in conference.";
		return;
	}

	if (!isAnotherSessionInState(session, State::Ringback)) {
		doStopAllTones();
		doStartRingbackTone(session);
	}
}

void ToneManager::startRingtone(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__;
	printDebugInfo(session);
	setState(session, State::Ringtone);
	if (!isAnotherSessionInState(session, State::Ringtone) && !isAnotherSessionInState(session, State::Ringback)) {
		doStopAllTones();
		doStartRingtone(session);
		mStats->number_of_startRingtone++;
	}
}

void ToneManager::startErrorTone(const std::shared_ptr<CallSession> &session, LinphoneReason reason) {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();
	setState(session, State::Tone);
	if (linphone_core_tone_indications_enabled(lc)) {
		printDebugInfo(session);
		doStopAllTones();
		doStartErrorTone(session, reason);
		mStats->number_of_startErrorTone++;
	}
}

void ToneManager::startNamedTone(const std::shared_ptr<CallSession> &session, LinphoneToneID toneId) {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();
	setState(session, State::Tone);
	if (linphone_core_tone_indications_enabled(lc)) {
		printDebugInfo(session);
		doStopAllTones();
		doStartNamedTone(session, toneId);
		mStats->number_of_startNamedTone++;
	}
}

void ToneManager::goToCall(const std::shared_ptr<CallSession> &session) {
	printDebugInfo(session);
	lInfo() << "[ToneManager] " << __func__;
	doStop(session, State::Call);
}

void ToneManager::stop(const std::shared_ptr<CallSession> &session) {
	printDebugInfo(session);
	lInfo() << "[ToneManager] " << __func__;
	doStop(session, State::None);
}

void ToneManager::removeSession(const std::shared_ptr<CallSession> &session) {
	printDebugInfo(session);
	mSessions.erase(session);
	lInfo() << "[ToneManager] removeSession mSession size : " <<  mSessions.size();
}

/**
 * This start again ringtone when one call is not anymore in Ringtone state but the second call is still in this state
 * This code can't be called just after the doStopRingtone because the first call needs to change its context (deletion or start a call)
 */
void ToneManager::update(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__ << " State:" << session->getState();
	switch(session->getState()) {
		case CallSession::State::UpdatedByRemote:
		case CallSession::State::Updating:
// On Updating, restart the tone if another session is ringing
			printDebugInfo(session);
			if (isAnotherSessionInState(session, State::Ringtone)) {
				lInfo() << "[ToneManager] start again ringtone";
				doStartRingtone(session);
				mStats->number_of_startRingtone++;
			}
			break;
		case CallSession::State::Error:
		case CallSession::State::End:
// On release, play the a generic end of call tone
			doStop(session, State::None);	// Stop rings related to the sessions. startErrorTone will set the new state if a tone is played.
			if(linphone_core_tone_indications_enabled(getCore()->getCCore())) {
				auto state = session->getTransferState();
				if( state == CallSession::State::Connected)
					doStartErrorTone(session, LinphoneReasonTransferred);
				else
					doStartErrorTone(session, session->getReason());
				mStats->number_of_startErrorTone++;
			}
			break;
		case CallSession::State::StreamsRunning:
		case CallSession::State::Paused:
		case CallSession::State::PausedByRemote:
// Update current tones when pausing or when the current call is running
			setState(session, State::Call);
			updateRings();
			break;
		default:
			break;
	}
}

// ---------------------------------------------------
// linphone core public API entrypoints
// ---------------------------------------------------

void ToneManager::linphoneCorePlayDtmf(char dtmf, int duration) {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	std::shared_ptr<CallSession> session = nullptr;
	if (getSessionInState(State::Tone, session)) {
		doStop(session, State::None);
	}

	MSSndCard *card = linphone_core_in_call(lc)
		? lc->sound_conf.play_sndcard
		: lc->sound_conf.ring_sndcard;
	MSFilter *f = getAudioResource(ToneGenerator, card, true);

	if (f == NULL) {
		lError() << "[ToneManager] No dtmf generator at this time !";
		return;
	}

	if (duration > 0) {
		ms_filter_call_method(f, MS_DTMF_GEN_PLAY, &dtmf);
	} else {
		ms_filter_call_method(f, MS_DTMF_GEN_START, &dtmf);
	}
}

void ToneManager::linphoneCoreStopDtmf() {
	lInfo() << "[ToneManager] " << __func__;
	MSFilter *f = getAudioResource(ToneGenerator, NULL, false);
	if (f != NULL) {
		ms_filter_call_method_noarg (f, MS_DTMF_GEN_STOP);
	}
}

LinphoneStatus ToneManager::linphoneCorePlayLocal(const char *audiofile) {
	lInfo() << "[ToneManager] " << __func__;
	return playFile(audiofile);
}

void ToneManager::linphoneCoreStartDtmfStream() {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	/*make sure ring stream is started*/
	getAudioResource(ToneGenerator, lc->sound_conf.ring_sndcard, true);

	mDtmfStreamStarted = true;
}

void ToneManager::linphoneCoreStopRinging() {
	lInfo() << "[ToneManager] " << __func__;
	doStopRingtone(nullptr);
}

void ToneManager::linphoneCoreStopDtmfStream() {
	if (!mDtmfStreamStarted) return;
	lInfo() << "[ToneManager] " << __func__;

	stop();

	mDtmfStreamStarted = false;
}

void ToneManager::stop() {
	lInfo() << "[ToneManager] " << __func__;

	doStopTone();
}

// ---------------------------------------------------
// timer
// ---------------------------------------------------

void ToneManager::createTimerToCleanTonePlayer(unsigned int delay) {
	lInfo() << "[ToneManager] " << __func__ << " [" << delay << "ms]";
	if (!mTimer) {
		auto callback = [this] () -> bool {
			LinphoneCore *lc = getCore()->getCCore();
			auto source = lc->ringstream ? lc->ringstream->source : nullptr;
			MSPlayerState state;
			if (source && ms_filter_call_method(source, MS_PLAYER_GET_STATE, &state) == 0) {
				if (state != MSPlayerPlaying) {
					deleteTimer();
					return false;
				}else{
					return false;
				}
			}else if(!source) {// Stop the timer if there is no more ring stream. It can happen when forcing ring to stop while playing a tone. Deleting timer here avoid concurrency issue.
				deleteTimer();
				return false;
			}
			return true;
		};
		mTimer = getCore()->createTimer(callback, delay, "Tone player cleanup");
	}
}

void ToneManager::deleteTimer() {
	if (mTimer) {
		lInfo() << "[ToneManager] " << __func__;
		mStats->number_of_stopTone++;
		getCore()->destroyTimer(mTimer);
		mTimer = nullptr;
	}
}

// ---------------------------------------------------
// setup tones
// ---------------------------------------------------

LinphoneToneDescription *ToneManager::getToneFromReason(LinphoneReason reason) {
	const bctbx_list_t *elem;
	for (elem = getCore()->getCCore()->tones; elem != NULL; elem = elem->next) {
		LinphoneToneDescription *tone = (LinphoneToneDescription *) elem->data;
		if (tone->reason==reason) return tone;
	}
	return NULL;
}

LinphoneToneDescription *ToneManager::getToneFromId(LinphoneToneID id) {
	const bctbx_list_t *elem;
	for (elem = getCore()->getCCore()->tones; elem != NULL; elem = elem->next) {
		LinphoneToneDescription *tone = (LinphoneToneDescription *) elem->data;
		if (tone->toneid == id) return tone;
	}
	return NULL;
}

void ToneManager::setTone(LinphoneReason reason, LinphoneToneID id, const char *audiofile) {
	LinphoneCore *lc = getCore()->getCCore();
	LinphoneToneDescription *tone = getToneFromReason(reason);

	if (tone) {
		lc->tones = bctbx_list_remove(lc->tones, tone);
		linphone_tone_description_destroy(tone);
	}
	tone = linphone_tone_description_new(reason, id, audiofile);
	lc->tones = bctbx_list_append(lc->tones, tone);
}

// ---------------------------------------------------
// callbacks file player
// ---------------------------------------------------

static void on_file_player_end(void *userData, MSFilter *f, unsigned int eventId, void *arg) {
	ToneManager *toneManager = (ToneManager *) userData;
	toneManager->onFilePlayerEnd(eventId);
}

static void on_play_tone_end(void *userData, MSFilter *f, unsigned int eventId, void *arg) {
	ToneManager *toneManager = (ToneManager *) userData;
	toneManager->onPlayToneEnd(eventId);
}

void ToneManager::onFilePlayerEnd(unsigned int eventId) {
	switch (eventId) {
		case MS_PLAYER_EOF:
			lInfo() << "[ToneManager] " << __func__;
			doStopTone();
			mStats->number_of_stopTone++;
			updateRings();
			break;
		default:
			break;
	}
}

void ToneManager::onPlayToneEnd(unsigned int eventId) {
	lInfo() << "[ToneManager] " << __func__ << " [" << eventId << "]";
	switch (eventId) {
		case MS_DTMF_GEN_END:{
				if(!mTimer)
					mStats->number_of_stopTone++;// Is takken account by deleteTimer
				updateRings();
			}break;
		default:
			break;
	}
}

// UpdateRings is call after a tone end, or when updating call state
void ToneManager::updateRings(){
	lInfo() << "[ToneManager] " << __func__;
	std::shared_ptr<CallSession> session = nullptr;
	if( getSessionInState(State::Ringtone, session)) {// Check if we need to ring first
		doStartRingtone(session);
		mStats->number_of_startRingtone++;
	}else{
		if( getSessionInState(State::Ringback, session)) {// Check if a ringback must be hear
			LinphoneCore *lc = getCore()->getCCore();
			if (!lc->ringstream) {// a ringback is not already playing
				doStartRingbackTone(session);
				mStats->number_of_startRingbackTone++;
			}
		}
	}
}

// ---------------------------------------------------
// tester
// ---------------------------------------------------

LinphoneCoreToneManagerStats *ToneManager::getStats() {
	return mStats;
}

void ToneManager::resetStats() {
	*mStats = {0, 0, 0, 0, 0, 0, 0};
}

// ---------------------------------------------------
// sessions
// ---------------------------------------------------

/* set State for the session. Insert session if not present */
void ToneManager::setState(const std::shared_ptr<CallSession> &session, ToneManager::State newState) {
	if (mSessions.count(session) == 0) {
		lInfo() << "[ToneManager] add new session [" << session << "]";
	}
	mSessions[session] = newState;
}

ToneManager::State ToneManager::getState(const std::shared_ptr<CallSession> &session) {
	auto search = mSessions.find(session);
    if (search != mSessions.end()) {
        return search->second;
    } else {
		return State::None;
    }
}

bool ToneManager::isAnotherSessionInState(const std::shared_ptr<CallSession> &me, ToneManager::State state) {
	for (auto it = mSessions.begin(); it != mSessions.end(); it++) {
		if (it->second == state && it->first != me) {
			return true;
		}
	}
	return false;
}

bool ToneManager::getSessionInState(ToneManager::State state, std::shared_ptr<CallSession> &session) {
	for (auto it = mSessions.begin(); it != mSessions.end(); it++) {
		if (it->second == state) {
			session = it->first;
			return true;
		}
	}
	return false;
}

bool ToneManager::isThereACall() {
	for (auto it = mSessions.begin(); it != mSessions.end(); it++) {
		if (it->second == State::Call) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------
// start
// ---------------------------------------------------

void ToneManager::doStartErrorTone(const std::shared_ptr<CallSession> &session, LinphoneReason reason) {
	lInfo() << "[ToneManager] " << __func__ << " [" << Utils::toString(reason) << "]";
	LinphoneToneDescription *tone = getToneFromReason(reason);

	if (tone) {
		if (tone->audiofile) {
			playFile(tone->audiofile);
		} else if (tone->toneid != LinphoneToneUndefined) {
			MSDtmfGenCustomTone dtmfTone = generateToneFromId(tone->toneid);
			playTone(session, dtmfTone);
		}
	}
}

void ToneManager::doStartNamedTone(const std::shared_ptr<CallSession> &session, LinphoneToneID toneId) {
	lInfo() << "[ToneManager] " << __func__ << " [" << Utils::toString(toneId) << "]";
	LinphoneToneDescription *tone = getToneFromId(toneId);
	
	if (tone && tone->audiofile) {
		playFile(tone->audiofile);
	} else {
		MSDtmfGenCustomTone dtmfTone = generateToneFromId(toneId);
		playTone(session, dtmfTone);
	}
}

void ToneManager::doStartRingbackTone(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	if (!lc->sound_conf.play_sndcard)
		return;

	MSSndCard *ringCard = lc->sound_conf.lsd_card ? lc->sound_conf.lsd_card : lc->sound_conf.play_sndcard;

	std::shared_ptr<LinphonePrivate::Call> call = getCore()->getCurrentCall();
	if (call) {
		AudioDevice * audioDevice = call->getOutputAudioDevice();

		// If the user changed the audio device before the ringback started, the new value will be stored in the call playback card
		// It is NULL otherwise
		if (audioDevice) {
			ringCard = audioDevice->getSoundCard();
		}
	}

	if (lc->sound_conf.remote_ring) {
		ms_snd_card_set_stream_type(ringCard, MS_SND_CARD_STREAM_VOICE);
		lc->ringstream = ring_start(lc->factory, lc->sound_conf.remote_ring, 2000, (lc->use_files) ? NULL : ringCard);
	}

}

void ToneManager::doStartRingtone(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	if (isAnotherSessionInState(session, State::Call) || isAnotherSessionInState(session, State::Ringtone)) {
		/* play a tone within the context of the current call */
		if(linphone_core_tone_indications_enabled(lc))
			doStartNamedTone(session, LinphoneToneCallWaiting);
	} else {
		MSSndCard *ringcard = lc->sound_conf.lsd_card ? lc->sound_conf.lsd_card : lc->sound_conf.ring_sndcard;
		if (ringcard && !linphone_core_is_native_ringing_enabled(lc)) {
			if (!linphone_core_callkit_enabled(lc)){
				ms_snd_card_set_stream_type(ringcard, MS_SND_CARD_STREAM_RING);
				linphone_ringtoneplayer_start(lc->factory, lc->ringtoneplayer, ringcard, lc->sound_conf.local_ring, 2000);
			}else{
				ms_message("Callkit is enabled, not playing ringtone.");
			}
		}
	}
}

// ---------------------------------------------------
// stop
// ---------------------------------------------------

void ToneManager::doStop(const std::shared_ptr<CallSession> &session, ToneManager::State newState) {
	lInfo() << "[ToneManager] " << __func__ << " from " << stateToString(getState(session)) << " to " << stateToString(newState);
	switch (getState(session)) {
		case ToneManager::Ringback:
			doStopRingbackTone();
			setState(session, newState);
			mStats->number_of_stopRingbackTone++;
			break;
		case ToneManager::Ringtone:
			doStopRingtone(session);
			setState(session, newState);
			mStats->number_of_stopRingtone++;
			/* start again ringtone in update() in case another call is in Ringtone state */
			break;
		case ToneManager::Tone:
			doStopTone();
			setState(session, newState);
			mStats->number_of_stopTone++;
			break;
		case ToneManager::Call:
			if (isAnotherSessionInState(session, ToneManager::Ringtone)) {
				doStopTone();
				mStats->number_of_stopTone++;
			}
			setState(session, newState);
			break;
		default:
			lInfo() << "[ToneManager] nothing to stop";
			break;
	}
}

void ToneManager::doStopRingbackTone() {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	if (lc->ringstream) {
		ring_stop(lc->ringstream);
		lc->ringstream = NULL;
	}
}

void ToneManager::doStopTone() {
	lInfo() << "[ToneManager] " << __func__;
	LinphoneCore *lc = getCore()->getCCore();

	doStopRingbackTone();

	if (isThereACall()) {
		MSFilter *f = getAudioResource(LocalPlayer, lc->sound_conf.play_sndcard, false);
		if(f != NULL) ms_filter_call_method_noarg(f, MS_PLAYER_CLOSE);// MS_PLAYER is used while being in call
		f = getAudioResource(ToneGenerator, NULL, FALSE);
		if (f != NULL) ms_filter_call_method_noarg(f, MS_DTMF_GEN_STOP);
	}
}

void ToneManager::doStopAllTones() {
	lInfo() << "[ToneManager] " << __func__;
	doStopTone();
	LinphoneCore *lc = getCore()->getCCore();
	if (linphone_ringtoneplayer_is_started(lc->ringtoneplayer)) {
		linphone_ringtoneplayer_stop(lc->ringtoneplayer);
	}
}


void ToneManager::doStopRingtone(const std::shared_ptr<CallSession> &session) {
	lInfo() << "[ToneManager] " << __func__;
			
	if (isAnotherSessionInState(session, State::Call) ) {
		/* stop the tone within the context of the current call */
		doStopTone();
	} else {
		LinphoneCore *lc = getCore()->getCCore();
		if (linphone_ringtoneplayer_is_started(lc->ringtoneplayer)) {
			linphone_ringtoneplayer_stop(lc->ringtoneplayer);
		}
	}
}

// ---------------------------------------------------
// sound
// ---------------------------------------------------

LinphoneStatus ToneManager::playFile(const char *audiofile) {
	LinphoneCore *lc = getCore()->getCCore();
	MSFilter *f = getAudioResource(LocalPlayer, lc->sound_conf.play_sndcard, true);
	int loopms = -1;
	if (!f) return -1;
	ms_filter_call_method(f, MS_PLAYER_SET_LOOP, &loopms);
	if (ms_filter_call_method(f, MS_PLAYER_OPEN, (void*) audiofile) != 0) {
		return -1;
	}
	ms_filter_call_method_noarg(f, MS_PLAYER_START);

	if (lc->ringstream && lc->ringstream->source) {
		ms_filter_add_notify_callback(lc->ringstream->source, &on_file_player_end, this, FALSE);
	}
	return 0;
}

MSDtmfGenCustomTone ToneManager::generateToneFromId(LinphoneToneID toneId) {
	MSDtmfGenCustomTone def;
	memset(&def, 0, sizeof(def));
	def.amplitude = 1;
	/*these are french tones, excepted the failed one, which comes USA congestion on mono-frequency*/
	switch(toneId) {
		case LinphoneToneCallOnHold:
			def.repeat_count=3;
			def.duration=300;
			def.frequencies[0]=440;
			def.interval=2000;
		break;
		case LinphoneToneCallWaiting:
			def.duration=300;
			def.frequencies[0]=440;
			def.interval=2000;
		break;
		case LinphoneToneBusy:
			def.duration=500;
			def.frequencies[0]=440;
			def.interval=500;
			def.repeat_count=3;
		break;
		case LinphoneToneCallLost:
			def.duration=250;
			//def.frequencies[0]=480;  // Second frequency that is hide
			def.frequencies[0]=620;
			def.interval=250;
			def.repeat_count=3;
		break;
		case LinphoneToneCallEnd:
			def.duration=200;
			def.frequencies[0]=480;
			def.interval=200;
			def.repeat_count=2;
			def.amplitude = 0.5f;// This tone can be in parallel of other calls. This will be played on a lighter amplitude
		break;
		default:
			lWarning() << "[ToneManager] Unhandled tone id.";
	}
	return def;
}

void ToneManager::playTone(const std::shared_ptr<CallSession> &session, MSDtmfGenCustomTone tone) {
    LinphoneCore *lc = getCore()->getCCore();

	MSSndCard * card = NULL;

	// FIXME: Properly handle case where session is nullptr
	if (session) {
		AudioDevice * audioDevice = std::dynamic_pointer_cast<MediaSession>(session)->getPrivate()->getCurrentOutputAudioDevice();
		if (audioDevice) {
			card = audioDevice->getSoundCard();
		}
	}

	// If card is null, use the default playcard
	if (card == NULL) {
		card = lc->sound_conf.play_sndcard;
	}

	MSFilter *f = getAudioResource(ToneGenerator, card, true);
	if (f == NULL) {
		lError() << "[ToneManager] No tone generator at this time !";
		return;
	}
	if (tone.duration > 0) {
		ms_filter_call_method(f, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		
		ms_filter_remove_notify_callback(f,&on_play_tone_end, this );
		ms_filter_add_notify_callback(f, &on_play_tone_end, this, FALSE);
		if (tone.repeat_count > 0) {// Useless but keep it just to take account of audio destruction
			int delay = (tone.duration + tone.interval) * tone.repeat_count + 1000;
			createTimerToCleanTonePlayer((unsigned int) delay);
		}
	}
}

MSFilter *ToneManager::getAudioResource(AudioResourceType rtype, MSSndCard *card, bool create) {
	LinphoneCore *lc = getCore()->getCCore();
	LinphoneCall *call = linphone_core_get_current_call(lc);
	AudioStream *stream = NULL;
	RingStream *ringstream;
	MSFilter * audioResource = NULL;
	float tmp;
	if (call) {
		stream = reinterpret_cast<AudioStream *>(linphone_call_get_stream(call, LinphoneStreamTypeAudio));
	} else if (linphone_core_is_in_conference(lc)) {
		stream = linphone_conference_get_audio_stream(lc->conf_ctx);
	}
	if (stream) {
		if (rtype == ToneGenerator) audioResource = stream->dtmfgen;
		if (rtype == LocalPlayer) audioResource = stream->local_player;
	}
	if(!audioResource) {
		if (card && lc->ringstream && card != lc->ringstream->card) {
			ring_stop(lc->ringstream);
			lc->ringstream = NULL;
		}
		if (lc->ringstream == NULL) {
		#if TARGET_OS_IPHONE
			tmp = 0.007f;
		#else
			tmp = 0.1f;
		#endif
			float amp = linphone_config_get_float(lc->config, "sound", "dtmf_player_amp", tmp);
			MSSndCard *ringcard = NULL;
			
			if (!lc->use_files) {
				ringcard = lc->sound_conf.lsd_card
				? lc->sound_conf.lsd_card
				: card
					? card
					: lc->sound_conf.ring_sndcard;
				if (ringcard == NULL) return NULL;
				ms_snd_card_set_stream_type(ringcard, MS_SND_CARD_STREAM_DTMF);
			}
			if (!create) return NULL;
	
			ringstream = lc->ringstream = ring_start(lc->factory, NULL, 0, ringcard); // passing a NULL ringcard if core if lc->use_files is enabled
			ms_filter_call_method(lc->ringstream->gendtmf, MS_DTMF_GEN_SET_DEFAULT_AMPLITUDE, &amp);
		} else {
			ringstream = lc->ringstream;
		}
		if (rtype == ToneGenerator) audioResource = ringstream->gendtmf;
		if (rtype == LocalPlayer) audioResource = ringstream->source;
	}
	return audioResource;
}

LINPHONE_END_NAMESPACE
