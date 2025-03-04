// Copyright 2007-2022 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "AudioInput.h"

#include "API.h"
#include "AudioOutput.h"
#include "CELTCodec.h"
#include "MainWindow.h"
#include "MumbleProtocol.h"
#include "NetworkConfig.h"
#include "OpusCodec.h"
#include "PacketDataStream.h"
#include "PluginManager.h"
#include "ServerHandler.h"
#include "User.h"
#include "Utils.h"
#include "VoiceRecorder.h"
#include "Global.h"

#ifdef USE_RNNOISE
extern "C" {
#	include "rnnoise.h"
}
#endif

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>

#ifdef USE_RNNOISE
/// Clip the given float value to a range that can be safely converted into a short (without causing integer overflow)
static short clampFloatSample(float v) {
	return static_cast< short >(std::min(std::max(v, static_cast< float >(std::numeric_limits< short >::min())),
										 static_cast< float >(std::numeric_limits< short >::max())));
}
#endif

void Resynchronizer::addMic(short *mic) {
	bool drop = false;
	{
		std::unique_lock< std::mutex > l(m);
		micQueue.push_back(mic);
		switch (state) {
			case S0:
				state = S1a;
				break;
			case S1a:
				state = S2;
				break;
			case S1b:
				state = S2;
				break;
			case S2:
				state = S3;
				break;
			case S3:
				state = S4a;
				break;
			case S4a:
				state = S5;
				break;
			case S4b:
				drop = true;
				break;
			case S5:
				drop = true;
				break;
		}
		if (drop) {
			delete[] micQueue.front();
			micQueue.pop_front();
		}
	}
	if (bDebugPrintQueue) {
		if (drop)
			qWarning("Resynchronizer::addMic(): dropped microphone chunk due to overflow");
		printQueue('+');
	}
}

AudioChunk Resynchronizer::addSpeaker(short *speaker) {
	AudioChunk result;
	bool drop = false;
	{
		std::unique_lock< std::mutex > l(m);
		switch (state) {
			case S0:
				drop = true;
				break;
			case S1a:
				drop = true;
				break;
			case S1b:
				state = S0;
				break;
			case S2:
				state = S1b;
				break;
			case S3:
				state = S2;
				break;
			case S4a:
				state = S3;
				break;
			case S4b:
				state = S3;
				break;
			case S5:
				state = S4b;
				break;
		}
		if (drop == false) {
			result = AudioChunk(micQueue.front(), speaker);
			micQueue.pop_front();
		}
	}
	if (drop)
		delete[] speaker;
	if (bDebugPrintQueue) {
		if (drop)
			qWarning("Resynchronizer::addSpeaker(): dropped speaker chunk due to underflow");
		printQueue('-');
	}
	return result;
}

void Resynchronizer::reset() {
	if (bDebugPrintQueue)
		qWarning("Resetting echo queue");
	std::unique_lock< std::mutex > l(m);
	state = S0;
	while (!micQueue.empty()) {
		delete[] micQueue.front();
		micQueue.pop_front();
	}
}

Resynchronizer::~Resynchronizer() {
	reset();
}

void Resynchronizer::printQueue(char who) {
	unsigned int mic;
	{
		std::unique_lock< std::mutex > l(m);
		mic = static_cast< unsigned int >(micQueue.size());
	}
	std::string line;
	line.reserve(32);
	line += who;
	line += " Echo queue [";
	for (unsigned int i = 0; i < 5; i++)
		line += i < mic ? '#' : ' ';
	line += "]\r";
	// This relies on \r to retrace always on the same line, can't use qWarining
	printf("%s", line.c_str());
	fflush(stdout);
}

// Remember that we cannot use static member classes that are not pointers, as the constructor
// for AudioInputRegistrar() might be called before they are initialized, as the constructor
// is called from global initialization.
// Hence, we allocate upon first call.

QMap< QString, AudioInputRegistrar * > *AudioInputRegistrar::qmNew;
QString AudioInputRegistrar::current = QString();

AudioInputRegistrar::AudioInputRegistrar(const QString &n, int p) : name(n), priority(p), echoOptions() {
	if (!qmNew)
		qmNew = new QMap< QString, AudioInputRegistrar * >();
	qmNew->insert(name, this);
}

AudioInputRegistrar::~AudioInputRegistrar() {
	qmNew->remove(name);
}

AudioInputPtr AudioInputRegistrar::newFromChoice(QString choice) {
	if (!qmNew)
		return AudioInputPtr();

	if (!choice.isEmpty() && qmNew->contains(choice)) {
		Global::get().s.qsAudioInput = choice;
		current                      = choice;
		return AudioInputPtr(qmNew->value(current)->create());
	}
	choice = Global::get().s.qsAudioInput;
	if (qmNew->contains(choice)) {
		current = choice;
		return AudioInputPtr(qmNew->value(choice)->create());
	}

	AudioInputRegistrar *r = nullptr;
	foreach (AudioInputRegistrar *air, *qmNew)
		if (!r || (air->priority > r->priority))
			r = air;
	if (r) {
		current = r->name;
		return AudioInputPtr(r->create());
	}
	return AudioInputPtr();
}

bool AudioInputRegistrar::canExclusive() const {
	return false;
}

bool AudioInputRegistrar::isMicrophoneAccessDeniedByOS() {
	return false;
}

AudioInput::AudioInput() : opusBuffer(Global::get().s.iFramesPerPacket * (SAMPLE_RATE / 100)) {
	bDebugDumpInput         = Global::get().bDebugDumpInput;
	resync.bDebugPrintQueue = Global::get().bDebugPrintQueue;
	if (bDebugDumpInput) {
		outMic.open("raw_microphone_dump", std::ios::binary);
		outSpeaker.open("speaker_dump", std::ios::binary);
		outProcessed.open("processed_microphone_dump", std::ios::binary);
	}

	adjustBandwidth(Global::get().iMaxBandwidth, iAudioQuality, iAudioFrames, bAllowLowDelay);

	Global::get().iAudioBandwidth = getNetworkBandwidth(iAudioQuality, iAudioFrames);

	m_codec = Mumble::Protocol::AudioCodec::CELT_Alpha;

	activityState = ActivityStateActive;
	oCodec        = nullptr;
	opusState     = nullptr;
	cCodec        = nullptr;
	ceEncoder     = nullptr;

	oCodec = Global::get().oCodec;
	if (oCodec) {
		if (bAllowLowDelay && iAudioQuality >= 64000) { // > 64 kbit/s bitrate and low delay allowed
			opusState = oCodec->opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, nullptr);
			qWarning("AudioInput: Opus encoder set for low delay");
		} else if (iAudioQuality >= 32000) { // > 32 kbit/s bitrate
			opusState = oCodec->opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, nullptr);
			qWarning("AudioInput: Opus encoder set for high quality speech");
		} else {
			opusState = oCodec->opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, nullptr);
			qWarning("AudioInput: Opus encoder set for low quality speech");
		}

		oCodec->opus_encoder_ctl(opusState, OPUS_SET_VBR(0)); // CBR
	}

#ifdef USE_RNNOISE
	denoiseState = rnnoise_create(nullptr);
#endif

	qWarning("AudioInput: %d bits/s, %d hz, %d sample", iAudioQuality, iSampleRate, iFrameSize);
	iEchoFreq = iMicFreq = iSampleRate;

	iFrameCounter   = 0;
	iSilentFrames   = 0;
	iHoldFrames     = 0;
	iBufferedFrames = 0;

	bResetProcessor = true;

	bEchoMulti = false;

	sppPreprocess = nullptr;
	sesEcho       = nullptr;
	srsMic = srsEcho = nullptr;

	iEchoChannels = iMicChannels = 0;
	iEchoFilled = iMicFilled = 0;
	eMicFormat = eEchoFormat = SampleFloat;
	iMicSampleSize = iEchoSampleSize = 0;

	bPreviousVoice = false;

	bResetEncoder = true;

	pfMicInput = pfEchoInput = nullptr;

	iBitrate    = 0;
	dPeakSignal = dPeakSpeaker = dPeakMic = dPeakCleanMic = 0.0;

	if (Global::get().uiSession) {
		setMaxBandwidth(Global::get().iMaxBandwidth);
	}

	bRunning = true;

	connect(this, SIGNAL(doDeaf()), Global::get().mw->qaAudioDeaf, SLOT(trigger()), Qt::QueuedConnection);
	connect(this, SIGNAL(doMute()), Global::get().mw->qaAudioMute, SLOT(trigger()), Qt::QueuedConnection);
}

AudioInput::~AudioInput() {
	bRunning = false;
	wait();

	if (opusState) {
		oCodec->opus_encoder_destroy(opusState);
	}

#ifdef USE_RNNOISE
	if (denoiseState) {
		rnnoise_destroy(denoiseState);
	}
#endif

	if (ceEncoder) {
		cCodec->celt_encoder_destroy(ceEncoder);
	}

	if (sppPreprocess)
		speex_preprocess_state_destroy(sppPreprocess);
	if (sesEcho)
		speex_echo_state_destroy(sesEcho);

	if (srsMic)
		speex_resampler_destroy(srsMic);
	if (srsEcho)
		speex_resampler_destroy(srsEcho);

	delete[] pfMicInput;
	delete[] pfEchoInput;
}

bool AudioInput::isTransmitting() const {
	return bPreviousVoice;
};

#define IN_MIXER_FLOAT(channels)                                                                             \
	static void inMixerFloat##channels(float *RESTRICT buffer, const void *RESTRICT ipt, unsigned int nsamp, \
									   unsigned int N, quint64 mask) {                                       \
		const float *RESTRICT input = reinterpret_cast< const float * >(ipt);                                \
		const float m               = 1.0f / static_cast< float >(channels);                                 \
		Q_UNUSED(N);                                                                                         \
		Q_UNUSED(mask);                                                                                      \
		for (unsigned int i = 0; i < nsamp; ++i) {                                                           \
			float v = 0.0f;                                                                                  \
			for (unsigned int j = 0; j < channels; ++j)                                                      \
				v += input[i * channels + j];                                                                \
			buffer[i] = v * m;                                                                               \
		}                                                                                                    \
	}

#define IN_MIXER_SHORT(channels)                                                                             \
	static void inMixerShort##channels(float *RESTRICT buffer, const void *RESTRICT ipt, unsigned int nsamp, \
									   unsigned int N, quint64 mask) {                                       \
		const short *RESTRICT input = reinterpret_cast< const short * >(ipt);                                \
		const float m               = 1.0f / (32768.f * static_cast< float >(channels));                     \
		Q_UNUSED(N);                                                                                         \
		Q_UNUSED(mask);                                                                                      \
		for (unsigned int i = 0; i < nsamp; ++i) {                                                           \
			float v = 0.0f;                                                                                  \
			for (unsigned int j = 0; j < channels; ++j)                                                      \
				v += static_cast< float >(input[i * channels + j]);                                          \
			buffer[i] = v * m;                                                                               \
		}                                                                                                    \
	}

static void inMixerFloatMask(float *RESTRICT buffer, const void *RESTRICT ipt, unsigned int nsamp, unsigned int N,
							 quint64 mask) {
	const float *RESTRICT input = reinterpret_cast< const float * >(ipt);

	unsigned int chancount = 0;
	STACKVAR(unsigned int, chanindex, N);
	for (unsigned int j = 0; j < N; ++j) {
		if ((mask & (1ULL << j)) == 0) {
			continue;
		}
		chanindex[chancount] = j; // Use chancount as index into chanindex.
		++chancount;
	}

	const float m = 1.0f / static_cast< float >(chancount);
	for (unsigned int i = 0; i < nsamp; ++i) {
		float v = 0.0f;
		for (unsigned int j = 0; j < chancount; ++j) {
			v += input[i * N + chanindex[j]];
		}
		buffer[i] = v * m;
	}
}

static void inMixerShortMask(float *RESTRICT buffer, const void *RESTRICT ipt, unsigned int nsamp, unsigned int N,
							 quint64 mask) {
	const short *RESTRICT input = reinterpret_cast< const short * >(ipt);

	unsigned int chancount = 0;
	STACKVAR(unsigned int, chanindex, N);
	for (unsigned int j = 0; j < N; ++j) {
		if ((mask & (1ULL << j)) == 0) {
			continue;
		}
		chanindex[chancount] = j; // Use chancount as index into chanindex.
		++chancount;
	}

	const float m = 1.0f / static_cast< float >(chancount);
	for (unsigned int i = 0; i < nsamp; ++i) {
		float v = 0.0f;
		for (unsigned int j = 0; j < chancount; ++j) {
			v += static_cast< float >(input[i * N + chanindex[j]]);
		}
		buffer[i] = v * m;
	}
}

IN_MIXER_FLOAT(1)
IN_MIXER_FLOAT(2)
IN_MIXER_FLOAT(3)
IN_MIXER_FLOAT(4)
IN_MIXER_FLOAT(5)
IN_MIXER_FLOAT(6)
IN_MIXER_FLOAT(7)
IN_MIXER_FLOAT(8)
IN_MIXER_FLOAT(N)

IN_MIXER_SHORT(1)
IN_MIXER_SHORT(2)
IN_MIXER_SHORT(3)
IN_MIXER_SHORT(4)
IN_MIXER_SHORT(5)
IN_MIXER_SHORT(6)
IN_MIXER_SHORT(7)
IN_MIXER_SHORT(8)
IN_MIXER_SHORT(N)

#undef IN_MIXER_FLOAT
#undef IN_MIXER_SHORT

AudioInput::inMixerFunc AudioInput::chooseMixer(const unsigned int nchan, SampleFormat sf, quint64 chanmask) {
	inMixerFunc r = nullptr;

	if (chanmask != 0xffffffffffffffffULL) {
		if (sf == SampleFloat) {
			r = inMixerFloatMask;
		} else if (sf == SampleShort) {
			r = inMixerShortMask;
		}
		return r;
	}

	if (sf == SampleFloat) {
		switch (nchan) {
			case 1:
				r = inMixerFloat1;
				break;
			case 2:
				r = inMixerFloat2;
				break;
			case 3:
				r = inMixerFloat3;
				break;
			case 4:
				r = inMixerFloat4;
				break;
			case 5:
				r = inMixerFloat5;
				break;
			case 6:
				r = inMixerFloat6;
				break;
			case 7:
				r = inMixerFloat7;
				break;
			case 8:
				r = inMixerFloat8;
				break;
			default:
				r = inMixerFloatN;
				break;
		}
	} else {
		switch (nchan) {
			case 1:
				r = inMixerShort1;
				break;
			case 2:
				r = inMixerShort2;
				break;
			case 3:
				r = inMixerShort3;
				break;
			case 4:
				r = inMixerShort4;
				break;
			case 5:
				r = inMixerShort5;
				break;
			case 6:
				r = inMixerShort6;
				break;
			case 7:
				r = inMixerShort7;
				break;
			case 8:
				r = inMixerShort8;
				break;
			default:
				r = inMixerShortN;
				break;
		}
	}
	return r;
}

void AudioInput::initializeMixer() {
	int err;

	if (srsMic)
		speex_resampler_destroy(srsMic);
	if (srsEcho)
		speex_resampler_destroy(srsEcho);
	delete[] pfMicInput;
	delete[] pfEchoInput;

	if (iMicFreq != iSampleRate)
		srsMic = speex_resampler_init(1, iMicFreq, iSampleRate, 3, &err);

	iMicLength = (iFrameSize * iMicFreq) / iSampleRate;

	pfMicInput = new float[iMicLength];

	if (iEchoChannels > 0) {
		bEchoMulti = (Global::get().s.echoOption == EchoCancelOptionID::SPEEX_MULTICHANNEL);
		if (iEchoFreq != iSampleRate)
			srsEcho = speex_resampler_init(bEchoMulti ? iEchoChannels : 1, iEchoFreq, iSampleRate, 3, &err);
		iEchoLength    = (iFrameSize * iEchoFreq) / iSampleRate;
		iEchoMCLength  = bEchoMulti ? iEchoLength * iEchoChannels : iEchoLength;
		iEchoFrameSize = bEchoMulti ? iFrameSize * iEchoChannels : iFrameSize;
		pfEchoInput    = new float[iEchoMCLength];
	} else {
		srsEcho     = nullptr;
		pfEchoInput = nullptr;
	}

	uiMicChannelMask = Global::get().s.uiAudioInputChannelMask;

	// There is no channel mask setting for the echo canceller, so allow all channels.
	uiEchoChannelMask = 0xffffffffffffffffULL;

	imfMic  = chooseMixer(iMicChannels, eMicFormat, uiMicChannelMask);
	imfEcho = chooseMixer(iEchoChannels, eEchoFormat, uiEchoChannelMask);

	iMicSampleSize = static_cast< int >(iMicChannels * ((eMicFormat == SampleFloat) ? sizeof(float) : sizeof(short)));
	iEchoSampleSize =
		static_cast< int >(iEchoChannels * ((eEchoFormat == SampleFloat) ? sizeof(float) : sizeof(short)));

	bResetProcessor = true;

	qWarning("AudioInput: Initialized mixer for %d channel %d hz mic and %d channel %d hz echo", iMicChannels, iMicFreq,
			 iEchoChannels, iEchoFreq);
	if (uiMicChannelMask != 0xffffffffffffffffULL) {
		qWarning("AudioInput: using mic channel mask 0x%llx", static_cast< unsigned long long >(uiMicChannelMask));
	}
}

void AudioInput::addMic(const void *data, unsigned int nsamp) {
	while (nsamp > 0) {
		// Make sure we don't overrun the frame buffer
		const unsigned int left = qMin(nsamp, iMicLength - iMicFilled);

		// Append mix into pfMicInput frame buffer (converts 16bit pcm->float if necessary)
		imfMic(pfMicInput + iMicFilled, data, left, iMicChannels, uiMicChannelMask);

		iMicFilled += left;
		nsamp -= left;

		// If new samples are left offset data pointer to point at the first one for next iteration
		if (nsamp > 0) {
			if (eMicFormat == SampleFloat)
				data = reinterpret_cast< const float * >(data) + left * iMicChannels;
			else
				data = reinterpret_cast< const short * >(data) + left * iMicChannels;
		}

		if (iMicFilled == iMicLength) {
			// Frame complete
			iMicFilled = 0;

			// If needed resample frame
			float *pfOutput = srsMic ? (float *) alloca(iFrameSize * sizeof(float)) : nullptr;
			float *ptr      = srsMic ? pfOutput : pfMicInput;

			if (srsMic) {
				spx_uint32_t inlen  = iMicLength;
				spx_uint32_t outlen = iFrameSize;
				speex_resampler_process_float(srsMic, 0, pfMicInput, &inlen, pfOutput, &outlen);
			}

			// If echo cancellation is enabled the pointer ends up in the resynchronizer queue
			// and may need to outlive this function's frame
			short *psMic = iEchoChannels > 0 ? new short[iFrameSize] : (short *) alloca(iFrameSize * sizeof(short));

			// Convert float to 16bit PCM
			const float mul = 32768.f;
			for (int j = 0; j < iFrameSize; ++j)
				psMic[j] = static_cast< short >(qBound(-32768.f, (ptr[j] * mul), 32767.f));

			// If we have echo cancellation enabled...
			if (iEchoChannels > 0) {
				resync.addMic(psMic);
			} else {
				encodeAudioFrame(AudioChunk(psMic));
			}
		}
	}
}

void AudioInput::addEcho(const void *data, unsigned int nsamp) {
	while (nsamp > 0) {
		// Make sure we don't overrun the echo frame buffer
		const unsigned int left = qMin(nsamp, iEchoLength - iEchoFilled);

		if (bEchoMulti) {
			const unsigned int samples = left * iEchoChannels;

			if (eEchoFormat == SampleFloat) {
				for (unsigned int i = 0; i < samples; ++i)
					pfEchoInput[i + iEchoFilled * iEchoChannels] = reinterpret_cast< const float * >(data)[i];
			} else {
				// 16bit PCM -> float
				for (unsigned int i = 0; i < samples; ++i)
					pfEchoInput[i + iEchoFilled * iEchoChannels] =
						static_cast< float >(reinterpret_cast< const short * >(data)[i]) * (1.0f / 32768.f);
			}
		} else {
			// Mix echo channels (converts 16bit PCM -> float if needed)
			imfEcho(pfEchoInput + iEchoFilled, data, left, iEchoChannels, uiEchoChannelMask);
		}

		iEchoFilled += left;
		nsamp -= left;

		// If new samples are left offset data pointer to point at the first one for next iteration
		if (nsamp > 0) {
			if (eEchoFormat == SampleFloat)
				data = reinterpret_cast< const float * >(data) + left * iEchoChannels;
			else
				data = reinterpret_cast< const short * >(data) + left * iEchoChannels;
		}

		if (iEchoFilled == iEchoLength) {
			// Frame complete

			iEchoFilled = 0;

			// Resample if necessary
			float *pfOutput = srsEcho ? (float *) alloca(iEchoFrameSize * sizeof(float)) : nullptr;
			float *ptr      = srsEcho ? pfOutput : pfEchoInput;

			if (srsEcho) {
				spx_uint32_t inlen  = iEchoLength;
				spx_uint32_t outlen = iFrameSize;
				speex_resampler_process_interleaved_float(srsEcho, pfEchoInput, &inlen, pfOutput, &outlen);
			}

			short *outbuff = new short[iEchoFrameSize];

			// float -> 16bit PCM
			const float mul = 32768.f;
			for (int j = 0; j < iEchoFrameSize; ++j) {
				outbuff[j] = static_cast< short >(qBound(-32768.f, (ptr[j] * mul), 32767.f));
			}

			auto chunk = resync.addSpeaker(outbuff);
			if (!chunk.empty()) {
				encodeAudioFrame(chunk);
				delete[] chunk.mic;
				delete[] chunk.speaker;
			}
		}
	}
}

void AudioInput::adjustBandwidth(int bitspersec, int &bitrate, int &frames, bool &allowLowDelay) {
	frames        = Global::get().s.iFramesPerPacket;
	bitrate       = Global::get().s.iQuality;
	allowLowDelay = Global::get().s.bAllowLowDelay;

	if (bitspersec == -1) {
		// No limit
	} else {
		if (getNetworkBandwidth(bitrate, frames) > bitspersec) {
			if ((frames <= 4) && (bitspersec <= 32000))
				frames = 4;
			else if ((frames == 1) && (bitspersec <= 64000))
				frames = 2;
			else if ((frames == 2) && (bitspersec <= 48000))
				frames = 4;
			if (getNetworkBandwidth(bitrate, frames) > bitspersec) {
				do {
					bitrate -= 1000;
				} while ((bitrate > 8000) && (getNetworkBandwidth(bitrate, frames) > bitspersec));
			}
		}
	}
	if (bitrate <= 8000)
		bitrate = 8000;
}

void AudioInput::setMaxBandwidth(int bitspersec) {
	if (bitspersec == Global::get().iMaxBandwidth)
		return;

	int frames;
	int bitrate;
	bool allowLowDelay;
	adjustBandwidth(bitspersec, bitrate, frames, allowLowDelay);

	Global::get().iMaxBandwidth = bitspersec;

	if (bitspersec != -1) {
		if ((bitrate != Global::get().s.iQuality) || (frames != Global::get().s.iFramesPerPacket))
			Global::get().mw->msgBox(
				tr("Server maximum network bandwidth is only %1 kbit/s. Audio quality auto-adjusted to %2 "
				   "kbit/s (%3 ms)")
					.arg(bitspersec / 1000)
					.arg(bitrate / 1000)
					.arg(frames * 10));
	}

	AudioInputPtr ai = Global::get().ai;
	if (ai) {
		Global::get().iAudioBandwidth = getNetworkBandwidth(bitrate, frames);
		ai->iAudioQuality             = bitrate;
		ai->iAudioFrames              = frames;
		ai->bAllowLowDelay            = allowLowDelay;
		return;
	}

	ai.reset();

	Audio::stopInput();
	Audio::startInput();
}

int AudioInput::getNetworkBandwidth(int bitrate, int frames) {
	int overhead = 20 + 8 + 4 + 1 + 2 + (Global::get().s.bTransmitPosition ? 12 : 0)
				   + (NetworkConfig::TcpModeEnabled() ? 12 : 0) + frames;
	overhead *= (800 / frames);
	int bw = overhead + bitrate;

	return bw;
}

void AudioInput::resetAudioProcessor() {
	if (!bResetProcessor)
		return;

	int iArg;

	if (sppPreprocess)
		speex_preprocess_state_destroy(sppPreprocess);
	if (sesEcho)
		speex_echo_state_destroy(sesEcho);

	sppPreprocess = speex_preprocess_state_init(iFrameSize, iSampleRate);
	resync.reset();
	selectNoiseCancel();

	iArg = 1;
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_VAD, &iArg);
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC, &iArg);
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_DEREVERB, &iArg);

	iArg = 30000;
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC_TARGET, &iArg);

	float v = 30000.0f / static_cast< float >(Global::get().s.iMinLoudness);
	iArg    = iroundf(floorf(20.0f * log10f(v)));
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &iArg);

	iArg = -60;
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC_DECREMENT, &iArg);

	if (noiseCancel == Settings::NoiseCancelSpeex) {
		iArg = Global::get().s.iSpeexNoiseCancelStrength;
		speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &iArg);
	}

	if (iEchoChannels > 0) {
		int filterSize = iFrameSize * (10 + resync.getNominalLag());
		sesEcho        = speex_echo_state_init_mc(iFrameSize, filterSize, 1, bEchoMulti ? iEchoChannels : 1);
		iArg           = iSampleRate;
		speex_echo_ctl(sesEcho, SPEEX_ECHO_SET_SAMPLING_RATE, &iArg);
		speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, sesEcho);

		qWarning("AudioInput: ECHO CANCELLER ACTIVE");
	} else {
		sesEcho = nullptr;
	}

	bResetEncoder = true;

	bResetProcessor = false;
}

bool AudioInput::selectCodec() {
	bool useOpus = false;

	// Currently talking, use previous Opus status.
	if (bPreviousVoice) {
		useOpus = (m_codec == Mumble::Protocol::AudioCodec::Opus);
	} else {
		if (Global::get().bOpus || (Global::get().s.lmLoopMode == Settings::Local)) {
			useOpus = true;
		}
	}

	if (!useOpus) {
		CELTCodec *switchto = nullptr;
		if ((!Global::get().uiSession || (Global::get().s.lmLoopMode == Settings::Local))
			&& (!Global::get().qmCodecs.isEmpty())) {
			// Use latest for local loopback
			QMap< int, CELTCodec * >::const_iterator i = Global::get().qmCodecs.constEnd();
			--i;
			switchto = i.value();
		} else {
			// Currently talking, don't switch unless you must.
			if (cCodec && bPreviousVoice) {
				int v = cCodec->bitstreamVersion();
				if ((v == Global::get().iCodecAlpha) || (v == Global::get().iCodecBeta))
					switchto = cCodec;
			}
		}
		if (!switchto) {
			switchto = Global::get().qmCodecs.value(Global::get().bPreferAlpha ? Global::get().iCodecAlpha
																			   : Global::get().iCodecBeta);
			if (!switchto)
				switchto = Global::get().qmCodecs.value(Global::get().bPreferAlpha ? Global::get().iCodecBeta
																				   : Global::get().iCodecAlpha);
		}
		if (switchto != cCodec) {
			if (cCodec && ceEncoder) {
				cCodec->celt_encoder_destroy(ceEncoder);
				ceEncoder = nullptr;
			}
			cCodec = switchto;
			if (cCodec)
				ceEncoder = cCodec->encoderCreate();
		}

		if (!cCodec)
			return false;
	}

	Mumble::Protocol::AudioCodec previousCodec = m_codec;
	if (useOpus) {
		m_codec = Mumble::Protocol::AudioCodec::Opus;
	} else {
		if (!Global::get().uiSession) {
			m_codec = Mumble::Protocol::AudioCodec::CELT_Alpha;
		} else {
			int v = cCodec->bitstreamVersion();
			if (v == Global::get().iCodecAlpha) {
				m_codec = Mumble::Protocol::AudioCodec::CELT_Alpha;
			} else if (v == Global::get().iCodecBeta) {
				m_codec = Mumble::Protocol::AudioCodec::CELT_Beta;
			} else {
				qWarning() << "Couldn't find message type for codec version" << v;
			}
		}
	}

	if (m_codec != previousCodec) {
		iBufferedFrames = 0;
		qlFrames.clear();
		opusBuffer.clear();
	}

	return true;
}

void AudioInput::selectNoiseCancel() {
	noiseCancel = Global::get().s.noiseCancelMode;

	if (noiseCancel == Settings::NoiseCancelRNN || noiseCancel == Settings::NoiseCancelBoth) {
#ifdef USE_RNNOISE
		if (!denoiseState || iFrameSize != 480) {
			qWarning("AudioInput: Ignoring request to enable RNNoise: internal error");
			noiseCancel = Settings::NoiseCancelSpeex;
		}
#else
		qWarning("AudioInput: Ignoring request to enable RNNoise: Mumble was built without support for it");
		noiseCancel = Settings::NoiseCancelSpeex;
#endif
	}

	int iArg = 0;
	switch (noiseCancel) {
		case Settings::NoiseCancelOff:
			qWarning("AudioInput: Noise canceller disabled");
			break;
		case Settings::NoiseCancelSpeex:
			qWarning("AudioInput: Using Speex as noise canceller");
			iArg = 1;
			break;
		case Settings::NoiseCancelRNN:
			qWarning("AudioInput: Using RNNoise as noise canceller");
			break;
		case Settings::NoiseCancelBoth:
			iArg = 1;
			qWarning("AudioInput: Using RNNoise and Speex as noise canceller");
			break;
	}
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_DENOISE, &iArg);
}

int AudioInput::encodeOpusFrame(short *source, int size, EncodingOutputBuffer &buffer) {
	int len;
	if (!oCodec) {
		return 0;
	}

	if (bResetEncoder) {
		oCodec->opus_encoder_ctl(opusState, OPUS_RESET_STATE, nullptr);
		bResetEncoder = false;
	}

	oCodec->opus_encoder_ctl(opusState, OPUS_SET_BITRATE(iAudioQuality));

	len = oCodec->opus_encode(opusState, source, size, &buffer[0], static_cast< opus_int32 >(buffer.size()));
	const int tenMsFrameCount = (size / iFrameSize);
	iBitrate                  = (len * 100 * 8) / tenMsFrameCount;
	return len;
}

int AudioInput::encodeCELTFrame(short *psSource, EncodingOutputBuffer &buffer) {
	int len;
	if (!cCodec)
		return 0;

	if (bResetEncoder) {
		cCodec->celt_encoder_ctl(ceEncoder, CELT_RESET_STATE);
		bResetEncoder = false;
	}

	cCodec->celt_encoder_ctl(ceEncoder, CELT_SET_PREDICTION(0));

	cCodec->celt_encoder_ctl(ceEncoder, CELT_SET_VBR_RATE(iAudioQuality));
	len      = cCodec->encode(ceEncoder, psSource, &buffer[0],
                         qMin< int >(iAudioQuality / (8 * 100), static_cast< int >(buffer.size())));
	iBitrate = len * 100 * 8;

	return len;
}

void AudioInput::encodeAudioFrame(AudioChunk chunk) {
	int iArg;
	int i;
	float sum;
	short max;

	short *psSource;

	iFrameCounter++;

	// As Global::get().iTarget is not protected by any locks, we avoid race-conditions by
	// copying it once at this point and stick to whatever value it is here. Thus
	// if the value of Global::get().iTarget changes during the execution of this function,
	// it won't cause any inconsistencies and the change is reflected once this
	// function is called again.
	int voiceTargetID = Global::get().iTarget;

	if (!bRunning)
		return;

	sum = 1.0f;
	max = 1;
	for (i = 0; i < iFrameSize; i++) {
		sum += static_cast< float >(chunk.mic[i] * chunk.mic[i]);
		max = std::max(static_cast< short >(abs(chunk.mic[i])), max);
	}
	dPeakMic = qMax(20.0f * log10f(sqrtf(sum / static_cast< float >(iFrameSize)) / 32768.0f), -96.0f);
	dMaxMic  = max;

	if (chunk.speaker && (iEchoChannels > 0)) {
		sum = 1.0f;
		for (i = 0; i < iEchoFrameSize; ++i) {
			sum += static_cast< float >(chunk.speaker[i] * chunk.speaker[i]);
		}
		dPeakSpeaker = qMax(20.0f * log10f(sqrtf(sum / static_cast< float >(iFrameSize)) / 32768.0f), -96.0f);
	} else {
		dPeakSpeaker = 0.0;
	}

	QMutexLocker l(&qmSpeex);
	resetAudioProcessor();

	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_GET_AGC_GAIN, &iArg);
	float gainValue = static_cast< float >(iArg);
	if (noiseCancel == Settings::NoiseCancelSpeex || noiseCancel == Settings::NoiseCancelBoth) {
		iArg = Global::get().s.iSpeexNoiseCancelStrength - iArg;
		speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &iArg);
	}

	short psClean[iFrameSize];
	if (sesEcho && chunk.speaker) {
		speex_echo_cancellation(sesEcho, chunk.mic, chunk.speaker, psClean);
		psSource = psClean;
	} else {
		psSource = chunk.mic;
	}

#ifdef USE_RNNOISE
	// At the time of writing this code, RNNoise only supports a sample rate of 48000 Hz.
	if (noiseCancel == Settings::NoiseCancelRNN || noiseCancel == Settings::NoiseCancelBoth) {
		float denoiseFrames[480];
		for (int i = 0; i < 480; i++) {
			denoiseFrames[i] = psSource[i];
		}

		rnnoise_process_frame(denoiseState, denoiseFrames, denoiseFrames);

		for (int i = 0; i < 480; i++) {
			psSource[i] = clampFloatSample(denoiseFrames[i]);
		}
	}
#endif

	speex_preprocess_run(sppPreprocess, psSource);

	sum = 1.0f;
	for (i = 0; i < iFrameSize; i++)
		sum += static_cast< float >(psSource[i] * psSource[i]);
	float micLevel = sqrtf(sum / static_cast< float >(iFrameSize));
	dPeakSignal    = qMax(20.0f * log10f(micLevel / 32768.0f), -96.0f);

	if (bDebugDumpInput) {
		outMic.write(reinterpret_cast< const char * >(chunk.mic), iFrameSize * sizeof(short));
		if (chunk.speaker) {
			outSpeaker.write(reinterpret_cast< const char * >(chunk.speaker), iEchoFrameSize * sizeof(short));
		}
		outProcessed.write(reinterpret_cast< const char * >(psSource), iFrameSize * sizeof(short));
	}

	spx_int32_t prob = 0;
	speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_GET_PROB, &prob);
	fSpeechProb = static_cast< float >(prob) / 100.0f;

	// clean microphone level: peak of filtered signal attenuated by AGC gain
	dPeakCleanMic = qMax(dPeakSignal - gainValue, -96.0f);
	float level   = (Global::get().s.vsVAD == Settings::SignalToNoise) ? fSpeechProb : (1.0f + dPeakCleanMic / 96.0f);

	bool bIsSpeech = false;

	if (level > Global::get().s.fVADmax) {
		// Voice-activation threshold has been reached
		bIsSpeech = true;
	} else if (level > Global::get().s.fVADmin && bPreviousVoice) {
		// Voice-deactivation threshold has not yet been reached
		bIsSpeech = true;
	}

	if (!bIsSpeech) {
		iHoldFrames++;
		if (iHoldFrames < Global::get().s.iVoiceHold)
			// Hold mic open until iVoiceHold threshold is reached
			bIsSpeech = true;
	} else {
		iHoldFrames = 0;
	}

	if (Global::get().s.atTransmit == Settings::Continuous
		|| API::PluginData::get().overwriteMicrophoneActivation.load()) {
		// Continuous transmission is enabled
		bIsSpeech = true;
	} else if (Global::get().s.atTransmit == Settings::PushToTalk) {
		// PTT is enabled, so check if it is currently active
		bIsSpeech = Global::get().s.uiDoublePush
					&& ((Global::get().uiDoublePush < Global::get().s.uiDoublePush)
						|| (Global::get().tDoublePush.elapsed() < Global::get().s.uiDoublePush));
	}

	// If Global::get().iPushToTalk > 0 that means that we are currently in some sort of PTT action. For
	// instance this could mean we're currently whispering
	bIsSpeech = bIsSpeech || (Global::get().iPushToTalk > 0);

	ClientUser *p          = ClientUser::get(Global::get().uiSession);
	bool bTalkingWhenMuted = false;
	if (Global::get().s.bMute || ((Global::get().s.lmLoopMode != Settings::Local) && p && (p->bMute || p->bSuppress))
		|| Global::get().bPushToMute || (voiceTargetID < 0)) {
		bTalkingWhenMuted = bIsSpeech;
		bIsSpeech         = false;
	}

	if (bIsSpeech) {
		iSilentFrames = 0;
	} else {
		iSilentFrames++;
		if (iSilentFrames > 500)
			iFrameCounter = 0;
	}

	if (p) {
		if (!bIsSpeech)
			p->setTalking(Settings::Passive);
		else if (voiceTargetID == 0)
			p->setTalking(Settings::Talking);
		else
			p->setTalking(Settings::Shouting);
	}

	if (Global::get().uiSession != 0 && (Global::get().s.bTxAudioCue || Global::get().s.bTxMuteCue)) {
		AudioOutputPtr ao = Global::get().ao;

		if (ao) {
			if (Global::get().s.bTxAudioCue) {
				if (bIsSpeech && !bPreviousVoice) {
					ao->playSample(Global::get().s.qsTxAudioCueOn);
				} else if (!bIsSpeech && bPreviousVoice) {
					ao->playSample(Global::get().s.qsTxAudioCueOff);
				}
			}

			if (Global::get().s.bTxMuteCue && !Global::get().bPushToMute && !Global::get().s.bDeaf
				&& bTalkingWhenMuted) {
				if (!qetLastMuteCue.isValid() || qetLastMuteCue.elapsed() > MUTE_CUE_DELAY) {
					qetLastMuteCue.start();
					ao->playSample(Global::get().s.qsTxMuteCue);
				}
			}
		}
	}

	if (!bIsSpeech && !bPreviousVoice) {
		iBitrate = 0;

		if ((tIdle.elapsed() / 1000000ULL) > Global::get().s.iIdleTime) {
			activityState = ActivityStateIdle;
			tIdle.restart();
			if (Global::get().s.iaeIdleAction == Settings::Deafen && !Global::get().s.bDeaf) {
				emit doDeaf();
			} else if (Global::get().s.iaeIdleAction == Settings::Mute && !Global::get().s.bMute) {
				emit doMute();
			}
		}

		if (activityState == ActivityStateReturnedFromIdle) {
			activityState = ActivityStateActive;
			if (Global::get().s.iaeIdleAction != Settings::Nothing && Global::get().s.bUndoIdleActionUponActivity) {
				if (Global::get().s.iaeIdleAction == Settings::Deafen && Global::get().s.bDeaf) {
					emit doDeaf();
				} else if (Global::get().s.iaeIdleAction == Settings::Mute && Global::get().s.bMute) {
					emit doMute();
				}
			}
		}

		spx_int32_t increment = 0;
		speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &increment);
		return;
	} else {
		spx_int32_t increment = 12;
		speex_preprocess_ctl(sppPreprocess, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &increment);
	}

	if (bIsSpeech && !bPreviousVoice) {
		bResetEncoder = true;
	}

	tIdle.restart();

	EncodingOutputBuffer buffer;
	Q_ASSERT(buffer.size() >= static_cast< size_t >(iAudioQuality / 100 * iAudioFrames / 8));

	emit audioInputEncountered(psSource, iFrameSize, iMicChannels, SAMPLE_RATE, bIsSpeech);

	int len = 0;

	bool encoded = true;
	if (!selectCodec())
		return;

	if (m_codec == Mumble::Protocol::AudioCodec::CELT_Alpha || m_codec == Mumble::Protocol::AudioCodec::CELT_Beta) {
		len = encodeCELTFrame(psSource, buffer);
		if (len <= 0) {
			iBitrate = 0;
			qWarning() << "encodeCELTFrame failed" << iBufferedFrames << iFrameSize << len;
			return;
		}
		++iBufferedFrames;
	} else if (m_codec == Mumble::Protocol::AudioCodec::Opus) {
		encoded = false;
		opusBuffer.insert(opusBuffer.end(), psSource, psSource + iFrameSize);
		++iBufferedFrames;

		if (!bIsSpeech || iBufferedFrames >= iAudioFrames) {
			if (iBufferedFrames < iAudioFrames) {
				// Stuff frame to framesize if speech ends and we don't have enough audio
				// this way we are guaranteed to have a valid framecount and won't cause
				// a codec configuration switch by suddenly using a wildly different
				// framecount per packet.
				const int missingFrames = iAudioFrames - iBufferedFrames;
				opusBuffer.insert(opusBuffer.end(), iFrameSize * missingFrames, 0);
				iBufferedFrames += missingFrames;
				iFrameCounter += missingFrames;
			}

			Q_ASSERT(iBufferedFrames == iAudioFrames);

			len = encodeOpusFrame(&opusBuffer[0], iBufferedFrames * iFrameSize, buffer);
			opusBuffer.clear();
			if (len <= 0) {
				iBitrate = 0;
				qWarning() << "encodeOpusFrame failed" << iBufferedFrames << iFrameSize << len;
				iBufferedFrames = 0; // These are lost. Make sure not to mess up our sequence counter next flushCheck.
				return;
			}
			encoded = true;
		}
	}

	if (encoded) {
		flushCheck(QByteArray(reinterpret_cast< char * >(&buffer[0]), len), !bIsSpeech, voiceTargetID);
	}

	if (!bIsSpeech)
		iBitrate = 0;

	bPreviousVoice = bIsSpeech;
}

static void sendAudioFrame(gsl::span< const Mumble::Protocol::byte > encodedPacket) {
	ServerHandlerPtr sh = Global::get().sh;
	if (sh) {
		sh->sendMessage(encodedPacket.data(), encodedPacket.size());
	}
}

void AudioInput::flushCheck(const QByteArray &frame, bool terminator, int voiceTargetID) {
	qlFrames << frame;

	if (!terminator && iBufferedFrames < iAudioFrames)
		return;

	Mumble::Protocol::AudioData audioData;
	audioData.targetOrContext = voiceTargetID;
	audioData.isLastFrame     = terminator;

	if (terminator && Global::get().iPrevTarget > 0) {
		// If we have been whispering to some target but have just ended, terminator will be true. However
		// in the case of whispering this means that we just released the whisper key so this here is the
		// last audio frame that is sent for whispering. The whisper key being released means that Global::get().iTarget
		// is reset to 0 by now. In order to send the last whisper frame correctly, we have to use
		// Global::get().iPrevTarget which is set to whatever Global::get().iTarget has been before its last change.

		audioData.targetOrContext = Global::get().iPrevTarget;

		// We reset Global::get().iPrevTarget as it has fulfilled its purpose for this whisper-action. It'll be set
		// accordingly once the client whispers for the next time.
		Global::get().iPrevTarget = 0;
	}
	if (Global::get().s.lmLoopMode == Settings::Server) {
		audioData.targetOrContext = Mumble::Protocol::ReservedTargetIDs::SERVER_LOOPBACK;
	}

	audioData.usedCodec = m_codec;

	int frames      = iBufferedFrames;
	iBufferedFrames = 0;

	audioData.frameNumber = iFrameCounter - frames;

	if (Global::get().s.bTransmitPosition && Global::get().pluginManager && !Global::get().bCenterPosition
		&& Global::get().pluginManager->fetchPositionalData()) {
		Position3D currentPos = Global::get().pluginManager->getPositionalData().getPlayerPos();

		audioData.position[0] = currentPos.x;
		audioData.position[1] = currentPos.y;
		audioData.position[2] = currentPos.z;

		audioData.containsPositionalData = true;
	}

	if (m_codec == Mumble::Protocol::AudioCodec::Opus) {
		// In Opus mode we only expect a single frame per packet
		assert(qlFrames.size() == 1);

		audioData.payload = gsl::span< const Mumble::Protocol::byte >(
			reinterpret_cast< const Mumble::Protocol::byte * >(qlFrames[0].constData()), qlFrames[0].size());
	} else {
		// Legacy codecs (Speex or CELT) may use multiple frames for a single packet
		if (!m_legacyBuffer) {
			m_legacyBuffer = std::make_unique< Mumble::Protocol::byte[] >(Mumble::Protocol::MAX_UDP_PACKET_SIZE);
		}

		if (terminator) {
			qlFrames << QByteArray();
			++frames;
		}

		std::size_t offset = 0;
		for (int i = 0; i < frames; ++i) {
			const QByteArray &qba = qlFrames[0];
			unsigned char head    = static_cast< unsigned char >(qba.size());
			if (i < frames - 1)
				head |= 0x80;
			std::memcpy(m_legacyBuffer.get() + offset, &head, sizeof(head));
			offset += sizeof(head);
			std::memcpy(m_legacyBuffer.get() + offset, qba.constData(), qba.size());
			offset += qba.size();
		}

		audioData.payload = gsl::span< const Mumble::Protocol::byte >(m_legacyBuffer.get(), offset);
	}

	{
		ServerHandlerPtr sh = Global::get().sh;
		if (sh) {
			VoiceRecorderPtr recorder(sh->recorder);
			if (recorder) {
				recorder->getRecordUser().addFrame(audioData);
			}

			m_udpEncoder.setProtocolVersion(sh->uiVersion);
		}
	}

	if (Global::get().s.lmLoopMode == Settings::Local) {
		// Only add audio data to local loop buffer
		LoopUser::lpLoopy.addFrame(audioData);
	} else {
		// Encode audio frame and send out
		gsl::span< const Mumble::Protocol::byte > encodedAudioPacket = m_udpEncoder.encodeAudioPacket(audioData);

		sendAudioFrame(encodedAudioPacket);
	}

	qlFrames.clear();
}

bool AudioInput::isAlive() const {
	return isRunning();
}
