/* Copyright (C) 2011, Benjamin Jemlich <pcgod@users.sourceforge.net>
   Copyright (C) 2011, Filipe Coelho <falktx@gmail.com>
   Copyright (C) 2015, Mikkel Krautz <mikkel@krautz.dk>
   Copyright (C) 2018, Bernd Buschinski <b.buschinski@gmail.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "JackAudio.h"

#include "Global.h"


static JackAudioSystem * jasys = NULL;

// jackStatusToStringList converts a jack_status_t (a flag type
// that can contain multiple Jack statuses) to a QStringList.
QStringList jackStatusToStringList(jack_status_t status) {
	QStringList statusList;

	if ((status & JackFailure) != 0) {
		statusList << QLatin1String("JackFailure - overall operation failed");
	}
	if ((status & JackInvalidOption) != 0) {
		statusList << QLatin1String("JackInvalidOption - the operation contained an invalid or unsupported option");
	}
	if ((status & JackNameNotUnique) != 0)  {
		statusList << QLatin1String("JackNameNotUnique - the desired client name is not unique");
	}
	if ((status & JackServerStarted) != 0) {
		statusList << QLatin1String("JackServerStarted - the server was started as a result of this operation");
	}
	if ((status & JackServerFailed) != 0) {
		statusList << QLatin1String("JackServerFailed - unable to connect to the JACK server");
	}
	if ((status & JackServerError) != 0) {
		statusList << QLatin1String("JackServerError - communication error with the JACK server");
	}
	if ((status & JackNoSuchClient) != 0) {
		statusList << QLatin1String("JackNoSuchClient - requested client does not exist");
	}
	if ((status & JackLoadFailure) != 0) {
		statusList << QLatin1String("JackLoadFailure - unable to load initial client");
	}
	if ((status & JackInitFailure) != 0) {
		statusList << QLatin1String("JackInitFailure - unable to initialize client");
	}
	if ((status & JackShmFailure) != 0)  {
		statusList << QLatin1String("JackShmFailure - unable to access shared memory");
	}
	if ((status & JackVersionError) != 0) {
		statusList << QLatin1String("JackVersionError - client's protocol version does not match");
	}
	if ((status & JackBackendError) != 0) {
		statusList << QLatin1String("JackBackendError - a backend error occurred");
	}
	if ((status & JackClientZombie) != 0) {
		statusList << QLatin1String("JackClientZombie - client zombified");
	}

	return statusList;
}

class JackAudioInputRegistrar : public AudioInputRegistrar {
	public:
		JackAudioInputRegistrar();
		virtual AudioInput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
		virtual bool canEcho(const QString &) const;
};

class JackAudioOutputRegistrar : public AudioOutputRegistrar {
	public:
		JackAudioOutputRegistrar();
		virtual AudioOutput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
};

class JackAudioInit : public DeferInit {
	public:
		JackAudioInputRegistrar *airJackAudio;
		JackAudioOutputRegistrar *aorJackAudio;
		void initialize() {
			jasys = new JackAudioSystem();
			jasys->init_jack();
			jasys->qmWait.lock();
			jasys->qwcWait.wait(&jasys->qmWait, 1000);
			jasys->qmWait.unlock();
			if (jasys->bJackIsGood) {
				airJackAudio = new JackAudioInputRegistrar();
				aorJackAudio = new JackAudioOutputRegistrar();
			} else {
				airJackAudio = NULL;
				aorJackAudio = NULL;
				delete jasys;
				jasys = NULL;
			}
		}

		void destroy() {
			if (airJackAudio)
				delete airJackAudio;
			if (aorJackAudio)
				delete aorJackAudio;
			if (jasys) {
				jasys->close_jack();
				delete jasys;
				jasys = NULL;
			}
		}
};

static JackAudioInit jackinit; //unused

JackAudioSystem::JackAudioSystem()
	: bActive(false)
	, client(NULL)
	, in_port(NULL)
	, output_buffer(NULL)
	, iBufferSize(0)
	, bJackIsGood(false)
	, iSampleRate(0)
{
	if (g.s.qsJackAudioOutput.isEmpty()) {
		iOutPorts = 1;
	} else {
		iOutPorts = g.s.qsJackAudioOutput.toInt();
	}
	memset((void*)&out_ports, 0, sizeof(out_ports));
}

JackAudioSystem::~JackAudioSystem() {
}

void JackAudioSystem::init_jack() {

	output_buffer = NULL;
	jack_status_t status = static_cast<jack_status_t>(0);
	int err = 0;

	jack_options_t jack_option = g.s.bJackStartServer ? JackNullOption : JackNoStartServer;
	client = jack_client_open("mumble", jack_option, &status);

	if (client) {
		in_port = jack_port_register(client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		if (in_port == NULL) {
			qWarning("JackAudioSystem: unable to register 'input' port");
			close_jack();
			return;
		}

		bJackIsGood = true;
		iBufferSize = jack_get_buffer_size(client);
		iSampleRate = jack_get_sample_rate(client);

		setNumberOfOutPorts(iOutPorts);

		if (bJackIsGood == false) {
			close_jack();
			return;
		}

		err = jack_set_process_callback(client, process_callback, this);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to set process callback - jack_set_process_callback() returned %i", err);
			close_jack();
			return;
		}

		err = jack_set_sample_rate_callback(client, srate_callback, this);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to set sample rate callback - jack_set_sample_rate_callback() returned %i", err);
			close_jack();
			return;
		}

		err = jack_set_buffer_size_callback(client, buffer_size_callback, this);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to set buffer size callback - jack_set_buffer_size_callback() returned %i", err);
			close_jack();
			return;
		}

		jack_on_shutdown(client, shutdown_callback, this);

		// If we made it this far, then everything is okay
		qhInput.insert(QString(), tr("Hardware Ports"));
		qhOutput.insert(QString::number(1), tr("Mono"));
		qhOutput.insert(QString::number(2), tr("Stereo"));
		bJackIsGood = true;

	} else {
		QStringList errors = jackStatusToStringList(status);
		qWarning("JackAudioSystem: unable to open jack client due to %i errors:", errors.count());
		for (int i = 0; i < errors.count(); ++i) {
			qWarning("JackAudioSystem:  %s", qPrintable(errors.at(i)));
		}
		bJackIsGood = false;
		client = NULL;
	}
}

void JackAudioSystem::close_jack() {

	QMutexLocker lock(&qmWait);
	if (client) {
		int err = 0;
		err = jack_deactivate(client);
		if (err != 0)  {
			qWarning("JackAudioSystem: unable to remove client from the process graph - jack_deactivate() returned %i", err);
		}

		bActive = false;

		if (in_port != NULL) {
			err = jack_port_unregister(client, in_port);
			if (err != 0)  {
				qWarning("JackAudioSystem: unable to unregister in port - jack_port_unregister() returned %i", err);
			}
		}

		for (unsigned i = 0; i < iOutPorts; ++i) {
			if (out_ports[i] != NULL) {
				err = jack_port_unregister(client, out_ports[i]);
				if (err != 0)  {
					qWarning("JackAudioSystem: unable to unregister out port - jack_port_unregister() returned %i", err);
				}
			}
		}

		err = jack_client_close(client);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to to disconnect from the JACK server - jack_client_close() returned %i", err);
		}

		delete [] output_buffer;
		output_buffer = NULL;

		client = NULL;
	}
	bJackIsGood = false;
}


void JackAudioSystem::auto_connect_ports()
{
	if (g.s.bJackAutoConnect == false) {
		return;
	}

	const char **ports = NULL;
	int const wanted_out_flags = JackPortIsPhysical | JackPortIsOutput;
	int const wanted_in_flags = JackPortIsPhysical | JackPortIsInput;
	int err;
	unsigned int connected_out_ports = 0;
	unsigned int connected_in_ports = 0;

	ports = jack_get_ports(client, 0, "audio", JackPortIsPhysical);
	if (ports != NULL) {
		int i = 0;
		while (ports[i] != NULL) {
			jack_port_t * const port = jack_port_by_name(client, ports[i]);
			if (port == NULL)  {
				qWarning("JackAudioSystem: jack_port_by_name() returned an invalid port - skipping it");
				continue;
			}

			int const port_flags = jack_port_flags(port);

			if ((port_flags & wanted_out_flags) == wanted_out_flags && connected_in_ports < 1) {
				err = jack_connect(client, ports[i], jack_port_name(in_port));
				if (err != 0) {
					qWarning("JackAudioSystem: unable to connect port '%s' to '%s' - jack_connect() returned %i", ports[i], jack_port_name(in_port), err);
				} else {
					connected_in_ports++;
				}
			}
			else if ((port_flags & wanted_in_flags) == wanted_in_flags && connected_out_ports < iOutPorts) {
				err = jack_connect(client, jack_port_name(out_ports[connected_out_ports]), ports[i]);
				if (err != 0) {
					qWarning("JackAudioSystem: unable to connect port '%s' to '%s' - jack_connect() returned %i", jack_port_name(out_ports[connected_out_ports]), ports[i], err);
				} else {
					connected_out_ports++;
				}
			}

			++i;
		}
	}
}

void JackAudioSystem::activate()
{
	QMutexLocker lock(&qmWait);
	if (client) {
		if (bActive) {
			return;
		}

		int err = jack_activate(client);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to activate client - jack_activate() returned %i", err);
			bJackIsGood = false;
			return;
		}
		bActive = true;

		auto_connect_ports();
	}
}

int JackAudioSystem::process_callback(jack_nframes_t nframes, void *arg) {

	JackAudioSystem * const jas = static_cast<JackAudioSystem*>(arg);

	if (jas && jas->bJackIsGood) {
		AudioInputPtr ai = g.ai;
		AudioOutputPtr ao = g.ao;
		JackAudioInput * const jai = dynamic_cast<JackAudioInput *>(ai.get());
		JackAudioOutput * const jao = dynamic_cast<JackAudioOutput *>(ao.get());

		if (jai && jai->isRunning() && jai->iMicChannels > 0 && !jai->isFinished()) {
			QMutexLocker(&jai->qmMutex);
			void * input = jack_port_get_buffer(jas->in_port, nframes);
			if (input != NULL) {
				jai->addMic(input, nframes);
			}
		}

		if (jao && jao->isRunning() && jao->iChannels > 0 && !jao->isFinished()) {
			QMutexLocker(&jao->qmMutex);

			jack_default_audio_sample_t* port_buffers[JACK_MAX_OUTPUT_PORTS];
			for (unsigned int i = 0; i < jao->iChannels; ++i) {

				port_buffers[i] = (jack_default_audio_sample_t*)jack_port_get_buffer(jas->out_ports[i], nframes);
				if (port_buffers[i] == NULL) {
					return 1;
				}
			}

			jack_default_audio_sample_t * const buffer = jas->output_buffer;
			memset(buffer, 0, sizeof(jack_default_audio_sample_t) * nframes * jao->iChannels);

			jao->mix(buffer, nframes);

			if (jao->iChannels == 1) {

				memcpy(port_buffers[0], buffer, sizeof(jack_default_audio_sample_t) * nframes);
			} else {

				// de-interleave channels
				for (unsigned int i = 0; i < nframes * jao->iChannels; ++i) {
					port_buffers[i % jao->iChannels][i / jao->iChannels] = buffer[i];
				}
			}
		}
	}

	return 0;
}

int JackAudioSystem::srate_callback(jack_nframes_t frames, void *arg) {

	JackAudioSystem * const jas = static_cast<JackAudioSystem*>(arg);
	jas->iSampleRate = frames;
	return 0;
}

void JackAudioSystem::allocOutputBuffer(jack_nframes_t frames) {

	iBufferSize = frames;
	AudioOutputPtr ao = g.ao;
	JackAudioOutput * const jao = dynamic_cast<JackAudioOutput *>(ao.get());

	if (jao) {
		jao->qmMutex.lock();
	}
	if (output_buffer) {
		delete [] output_buffer;
		output_buffer = NULL;
	}
	output_buffer = new jack_default_audio_sample_t[frames * numberOfOutPorts()];
	if (output_buffer == NULL) {
		bJackIsGood = false;
	}

	if (jao) {
		jao->qmMutex.unlock();
	}
}

void JackAudioSystem::setNumberOfOutPorts(unsigned int ports) {

	AudioOutputPtr ao = g.ao;
	JackAudioOutput * const jao = dynamic_cast<JackAudioOutput *>(ao.get());
	unsigned int const oldSize = iOutPorts;
	int err = 0;

	iOutPorts = qBound<unsigned>(1, ports, JACK_MAX_OUTPUT_PORTS);

	allocOutputBuffer(iBufferSize);

	if (jao) {
		jao->qmMutex.lock();
	}

	if (bActive) {
		err = jack_deactivate(client);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to remove client from the process graph - jack_deactivate() returned %i", err);
		}
	}

	for (unsigned int i = 0; i < oldSize; ++i) {
		if (out_ports[i] != NULL) {
			err = jack_port_unregister(client, out_ports[i]);
			if (err != 0)  {
				qWarning("JackAudioSystem: unable to unregister out port - jack_port_unregister() returned %i", err);
			}
			out_ports[i] = NULL;
		}
	}

	for (unsigned int i = 0; i < iOutPorts; ++i) {

		char name[10];
		snprintf(name, 10, "output_%d", i + 1);

		out_ports[i] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (out_ports[i] == NULL) {
			qWarning("JackAudioSystem: unable to register 'output' port");
			bJackIsGood = false;
			break;
		}
	}

	if (bActive) {
		err = jack_activate(client);
		if (err != 0) {
			qWarning("JackAudioSystem: unable to activate client - jack_activate() returned %i", err);
			bJackIsGood = false;
		}
	}

	if (jao) {
		jao->qmMutex.unlock();
	}
}

unsigned int JackAudioSystem::numberOfOutPorts() const {

	return iOutPorts;
}

int JackAudioSystem::buffer_size_callback(jack_nframes_t frames, void *arg) {

	JackAudioSystem * const jas = static_cast<JackAudioSystem*>(arg);
	jas->allocOutputBuffer(frames);
	return 0;
}

void JackAudioSystem::shutdown_callback(void *arg) {

	JackAudioSystem * const jas = static_cast<JackAudioSystem*>(arg);
	jas->bJackIsGood = false;
}

JackAudioInputRegistrar::JackAudioInputRegistrar() : AudioInputRegistrar(QLatin1String("JACK"), 10) {
}

AudioInput *JackAudioInputRegistrar::create() {
	return new JackAudioInput();
}

const QList<audioDevice> JackAudioInputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	QStringList qlInputDevs = jasys->qhInput.keys();
	qSort(qlInputDevs);

	foreach(const QString &dev, qlInputDevs) {
		qlReturn << audioDevice(jasys->qhInput.value(dev), dev);
	}

	return qlReturn;
}

void JackAudioInputRegistrar::setDeviceChoice(const QVariant &choice, Settings &s) {
	Q_UNUSED(choice);
	Q_UNUSED(s);
}

bool JackAudioInputRegistrar::canEcho(const QString &osys) const {
	Q_UNUSED(osys);
	return false;
}

JackAudioOutputRegistrar::JackAudioOutputRegistrar() : AudioOutputRegistrar(QLatin1String("JACK"), 10) {
}

AudioOutput *JackAudioOutputRegistrar::create() {
	return new JackAudioOutput();
}

const QList<audioDevice> JackAudioOutputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	QStringList qlOutputDevs = jasys->qhOutput.keys();
	qSort(qlOutputDevs);

	if (qlOutputDevs.contains(g.s.qsJackAudioOutput)) {
		qlOutputDevs.removeAll(g.s.qsJackAudioOutput);
		qlOutputDevs.prepend(g.s.qsJackAudioOutput);
	}

	foreach(const QString &dev, qlOutputDevs) {
		qlReturn << audioDevice(jasys->qhOutput.value(dev), dev);
	}

	return qlReturn;
}

void JackAudioOutputRegistrar::setDeviceChoice(const QVariant &choice, Settings &s) {

	s.qsJackAudioOutput = choice.toString();
	jasys->setNumberOfOutPorts(choice.toInt());
}

JackAudioInput::JackAudioInput() {
	bRunning = true;
	iMicChannels = 0;
}

JackAudioInput::~JackAudioInput() {
	bRunning = false;
	iMicChannels = 0;
	qmMutex.lock();
	qwcWait.wakeAll();
	qmMutex.unlock();
	wait();
}

void JackAudioInput::run() {
	if (jasys && jasys->bJackIsGood) {
		iMicFreq = jasys->iSampleRate;
		iMicChannels = 1;
		eMicFormat = SampleFloat;
		initializeMixer();
		jasys->activate();
	}

	qmMutex.lock();
	while (bRunning)
		qwcWait.wait(&qmMutex);
	qmMutex.unlock();
}

JackAudioOutput::JackAudioOutput() {
	bRunning = true;
	iChannels = 0;
}

JackAudioOutput::~JackAudioOutput() {
	bRunning = false;
	iChannels = 0;
	qmMutex.lock();
	qwcWait.wakeAll();
	qmMutex.unlock();
	wait();
}

void JackAudioOutput::run() {
	if (jasys && jasys->bJackIsGood) {
		unsigned int chanmasks[32];

		chanmasks[0] = SPEAKER_FRONT_LEFT;
		chanmasks[1] = SPEAKER_FRONT_RIGHT;

		eSampleFormat = SampleFloat;
		iChannels = jasys->numberOfOutPorts();
		iMixerFreq = jasys->iSampleRate;
		initializeMixer(chanmasks);
		jasys->activate();
	}

	qmMutex.lock();
	while (bRunning)
		qwcWait.wait(&qmMutex);
	qmMutex.unlock();
}
