
#include <thread>

#include "server.h"
#include "xptools/Socket.h"
#include "log/log.h"
#include "srbinding.h"

uint64_t g_session_start_ms;
uint32_t g_seqnum = 0;
double g_lastReportedRate;

uint64_t get_ms() {
	auto millisec_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return millisec_since_epoch;
}

float InterpolateTriggerTime(struct sr_channel *ch, uint8_t* buf, uint64_t trigpos, bool try_fix = true)
{
	if (trigpos <= 0) {
		return 999;
	}

	// These are all already in ADC values, so no need to scale
	uint8_t trigvalue = ch->trig_value;

	uint8_t pretrig = buf[trigpos-1];
	uint8_t afttrig = buf[trigpos];
	float slope = afttrig - pretrig;
	float delta = trigvalue - pretrig;
	float phase = (delta / slope);
	float final = - ( 1 - phase ); // ADC values are 'upside down'

	if (final <= -1 || final > 0) {
		// This means that the signal did not actually cross the trigger at the reported position.
		// Need to find the actual trigger position and shift by more than one sample when this happens

		if (try_fix) {
			// Scan forwards and backwards in the sample stream by up to this number of samples before
			// giving up:
			const int try_up_to = 10;
			int i = 1;
			while ( i < try_up_to ) {
				float res = InterpolateTriggerTime(ch, buf, trigpos + i, false);

				if (res > -1 && res <= 0) {
					// Success! The threshold was passed during the window offset by i samples; so
					// shift the trigphase of the waveform by that many clocks (trigphase is in
					// units of samples on this side of the bridge).
					return res + i;
				}

				if (i > 0) i = -i;
				else i = -i + 1;
			}

			LogWarning("Something has gone wrong in trigphase and couldn't be fixed (phase=%f)\n", phase);

		}

		return 999;
	}

	return final;
}

bool g_pendingAcquisition = false;

void waveform_callback (const struct sr_dev_inst *device, const struct sr_datafeed_packet *packet, void* client_vp) {
	Socket* client = (Socket*) client_vp;

	if (packet->type == SR_DF_HEADER) {
		struct sr_datafeed_header* header = (struct sr_datafeed_header*)packet->payload;
		(void) header;

	} else if (packet->type == SR_DF_END) {
		LogDebug("SR_DF_END; Capture Ended\n");

	} else if (packet->type == SR_DF_TRIGGER) {
		struct ds_trigger_pos* trigger = (struct ds_trigger_pos*)packet->payload;
		(void) trigger;

		// if (trigger->status & 0x01) {
  //           uint32_t trig_pos = trigger->real_pos;

  //           g_lastTrigPos = trig_pos * 2 / count_enabled_channels();

  //           LogWarning("Trigger packet; real_pos=%d, g_lastTrigPos=%d\n", trig_pos, g_lastTrigPos);
		// }

	} else if (packet->type == SR_DF_DSO) {
		uint32_t seqnum = g_seqnum++;

		if (!g_run) {
			// LogWarning("SR_DF_DSO: !g_run; ignoring\n");
			return;
		}
		// Don't send further data packets after stop requested

		if (!g_pendingAcquisition) {
			// LogWarning("SR_DF_DSO: !g_pendingAcquisition; ignoring to avoid buffering\n");
			return;
		}

		g_pendingAcquisition = false;
		g_capturedFirstFrame = true;

		struct sr_datafeed_dso* dso = (struct sr_datafeed_dso*)packet->payload;

		vector<int> sample_channels;
		int i = 0;
		for (GSList *l = device->channels; l != NULL; l = l->next) {
			// Should be able to do dso->probes but that appears to just always contain all channels,
			//  so do this instead.
			if (((struct sr_channel*)l->data)->enabled) {
        		sample_channels.push_back(i);
			}
        	i++;
        }

        uint16_t numchans = sample_channels.size();

		size_t num_samples = dso->num_samples;

		uint8_t* buf = (uint8_t*) dso->data;

		uint8_t* deinterleaving_buffer = NULL;
		uint8_t* buffer_for_trigphase = buf;
		if (numchans != 1) {
			deinterleaving_buffer = new uint8_t[numchans * num_samples];
			uint8_t* p = buf;
			for (size_t sample = 0; sample < num_samples; sample++) {
				for (int ch = 0; ch < numchans; ch++) {
					deinterleaving_buffer[(ch * num_samples) + sample] = *p;
					p++;
				}
			}

			buffer_for_trigphase = &deinterleaving_buffer[g_selectedTriggerChannel * num_samples];
		}

		uint64_t samplerate_hz = get_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE).value();

		uint64_t trigpos_in_samples = num_samples * g_trigpct / 100;

		if (samplerate_hz == 1000000000 && numchans == 2) {
			// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
			samplerate_hz /= 2;
		}

		float trigphase = InterpolateTriggerTime(g_channels[g_selectedTriggerChannel], buffer_for_trigphase, trigpos_in_samples);
		if (trigphase == 999) trigphase = 0;
		// trigphase needs to come from the channel that the trigger is on for all channels.
		// TODO: does this mean we need to offset the other channel by samplerate_fs/2 though if the
		// ADC sample is 180deg out of phase?

		client->SendLooped((uint8_t*)&seqnum, sizeof(seqnum));

		client->SendLooped((uint8_t*)&numchans, sizeof(numchans));

		int64_t samplerate_fs = 1000000000000000 / samplerate_hz;
		client->SendLooped((uint8_t*)&samplerate_fs, sizeof(samplerate_fs));

		double delta_s = ((double)(get_ms() - g_session_start_ms)) / 1000;

		if ((delta_s - g_lastReportedRate) > 10) {
			g_lastReportedRate = delta_s;

			double wfms_s = seqnum / delta_s;
			LogDebug("SR_DF_DSO: Seq#%u: %lu samples on %d channels, HW WFMs/s=%f\n", seqnum, num_samples, numchans, wfms_s);
		}

		int chindex = 0;
		for (size_t chnum : sample_channels) {
			//Send channel ID, scale, offset, and memory depth
			client->SendLooped((uint8_t*)&chnum, sizeof(chnum));
			client->SendLooped((uint8_t*)&num_samples, sizeof(num_samples));

			struct sr_channel* ch = g_channels[chnum];

			float scale, offset;
			compute_scale_and_offset(ch, scale, offset);

			if (deinterleaving_buffer)
				buf = &deinterleaving_buffer[chindex * num_samples];

			float config[3] = {scale, offset, trigphase};
			client->SendLooped((uint8_t*)&config, sizeof(config));

			//Send the actual waveform data
			client->SendLooped((uint8_t*)buf, num_samples * sizeof(int8_t));

			chindex++;
		}

		if (deinterleaving_buffer)
			delete[] deinterleaving_buffer;

		if (g_oneShot) {
			LogDebug("Stopping after oneshot\n");
			g_run = false;
			sr_session_stop();
		}
	}
}

void syncWaitThread(Socket* client) {
	for (;;) {
		uint8_t r = '0';
		client->RecvLooped(&r, 1);
		if (r != 'K') {
			// Disconnected
			return;
		}

		g_pendingAcquisition = true;
	}
}

void WaveformServerThread()
{
	#ifdef __linux__
	pthread_setname_np(pthread_self(), "WaveformServerThread");
	#endif

	Socket client = g_dataSocket.Accept();
	LogVerbose("Client connected to data plane socket\n");

	if(!client.IsValid())
		return;
	if(!client.DisableNagle())
		LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

	sr_session_datafeed_callback_add(waveform_callback, &client);

	std::thread dataThread(syncWaitThread, &client);

	for (;;) {
		if (g_quit) {
			return;
		} else if (!g_run) {
			usleep(100);
			continue;
		}

		LogDebug("Starting Session...\n");

		g_running = true;
		g_capturedFirstFrame = false;
		g_seqnum = 0;
		g_session_start_ms = get_ms();
		g_lastReportedRate = 0;

		int err;
		if ((err = sr_session_start()) != SR_OK) {
			LogError("session_start returned failure: %d\n", err);
			return;
		}

		// force_correct_sample_config();

		if ((err = sr_session_run()) != SR_OK) {
			LogError("session_run returned failure: %d\n", err);
			return;
		}

		g_running = false;

		LogDebug("Session Stopped.\n");
	}
}
