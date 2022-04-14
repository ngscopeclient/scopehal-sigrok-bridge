
#include <thread>
#include <atomic>

#include "server.h"
#include "xptools/Socket.h"
#include "log/log.h"
#include "srbinding.h"

uint64_t g_session_start_ms;
uint32_t g_hwSeqNum = 0;
double g_lastReportedRate;
uint32_t g_lastTrigPos;

int64_t g_currSeqNum = 0;
std::atomic<int64_t> g_lastAcked = 0;
std::atomic<int64_t> g_windowSize = 1;

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
		// LogDebug("SR_DF_END; Capture Ended\n");

	} else if (packet->type == SR_DF_TRIGGER) {
		struct ds_trigger_pos* trigger = (struct ds_trigger_pos*)packet->payload;
		(void) trigger;

		if (trigger->status & 0x01) {
            uint32_t trig_pos = trigger->real_pos;

            g_lastTrigPos = trig_pos;
		}
	} else if (packet->type == SR_DF_LOGIC || packet->type == SR_DF_DSO) {
		uint32_t hwSeqnum = g_hwSeqNum++;

		g_capturedFirstFrame = true;

		if (!g_run) {
			LogWarning("Feed: !g_run; ignoring\n");
			return;
		}
		// Don't send further data packets after stop requested

		int64_t outstanding = g_currSeqNum - g_lastAcked;

		// LogDebug("curr = %d, lastAcked = %d, outstanding = %d, wsize = %d\n", (int)g_currSeqNum, (int)g_lastAcked, (int)outstanding, (int)g_windowSize);

		if (outstanding >= g_windowSize) {
			// Drop to avoid overfilling TCP buffer
			// LogDebug(" (drop)\n");
			return;
		}

		g_currSeqNum++;

		vector<int> sample_channels;
		int chindex = 0;
		for (GSList *l = device->channels; l != NULL; l = l->next) {
			// Should be able to do dso->probes but that appears to just always contain all channels,
			//  so do this instead.
			// Note that on the DSLogic in non-stream mode (which we use because we want pretrigger buffer)
			//  the probes will all stay enabled according to this.
			if (((struct sr_channel*)l->data)->enabled) {
        		sample_channels.push_back(chindex);
			}
        	chindex++;
        }

        uint64_t samplerate_hz = get_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE).value();
        uint16_t numchans = sample_channels.size();

        if (samplerate_hz == 1000000000 && numchans == 2) {
			// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
			samplerate_hz /= 2;
		}

        size_t num_samples;
        vector<uint8_t*> deinterleaved_buffers;
        float trigphase = 0;
        int32_t first_sample = 0;
        vector<bool> clipping;
        for (int ch = 0; ch < numchans; ch++) clipping.push_back(false);

		if (packet->type == SR_DF_LOGIC) {
			struct sr_datafeed_logic* logic = (struct sr_datafeed_logic*)packet->payload;

			if (logic->data_error != 0) {
				LogWarning("SR_DF_LOGIC: data_error\n");
				return;
			}

			// // logic->index, ->order, ->unit_size are just not initialized in libsigrok4DSL code...
			// // ->format is always LA_CROSS_DATA
			// // ->length appears to be in bytes

			// // For N channels, yields 8 samples for each of the channels, then repeats
			// // Each sample is 8 bits, with the most significant bit sampled last

			num_samples = logic->length / numchans; // u8s per channel
			for (int ch = 0; ch < numchans; ch++) {
				deinterleaved_buffers.push_back(new uint8_t[num_samples]);
			}

			uint8_t* p = (uint8_t*)logic->data;
			for (size_t sample = 0; sample < num_samples; sample+=8) {
				for (int ch = 0; ch < numchans; ch++) {
					for (int i = 0; i < 8; i++) {
						deinterleaved_buffers[ch][sample + i] = *(p++);
					}
				}
			}

        	uint32_t nominal_trigpos_in_bits = num_samples * 8 * g_trigpct / 100;
        	// Where in the bitstream SHOULD the trigger be

        	uint32_t trigpos_in_bits = g_lastTrigPos * 8 * 2 / count_enabled_channels();
        	// Where in the bitstream DID the trigger happen

			first_sample = nominal_trigpos_in_bits - trigpos_in_bits;

		} else { // DSO
			struct sr_datafeed_dso* dso = (struct sr_datafeed_dso*)packet->payload;

			num_samples = dso->num_samples;

			uint8_t* buf = (uint8_t*) dso->data;

			for (int i = 0; i < numchans; i++) {
				deinterleaved_buffers.push_back(new uint8_t[num_samples]);
			}

			uint8_t* p = buf;
			for (size_t sample = 0; sample < num_samples; sample++) {
				for (int ch = 0; ch < numchans; ch++) {
					uint8_t d = *p;
					if (d <= g_hwmin || d >= g_hwmax) {
						clipping[ch] = true;
					}

					deinterleaved_buffers[ch][sample] = d;
					p++;
				}
			}

			// for (int i = 0; i < numchans; i++) {
			// 	printf(" ADC samples ch%d: ", i);
			// 	for (int x = 0; x < 16; x++) {
			// 		printf("%02x ", deinterleaved_buffers[i][x]);
			// 	}
			// 	printf("\n");
			// }

        	uint32_t nominal_trigpos_in_samples = num_samples * g_trigpct / 100;

			trigphase = InterpolateTriggerTime(g_channels[g_selectedTriggerChannel], deinterleaved_buffers[g_selectedTriggerChannel], nominal_trigpos_in_samples);
			if (trigphase == 999) trigphase = 0;
			// trigphase needs to come from the channel that the trigger is on for all channels.
			// TODO: does this mean we need to offset the other channel by samplerate_fs/2 though if the
			// ADC sample is 180deg out of phase?
		}

		client->SendLooped((uint8_t*)&g_currSeqNum, sizeof(g_currSeqNum));

		client->SendLooped((uint8_t*)&numchans, sizeof(numchans));

		int64_t samplerate_fs = 1000000000000000 / samplerate_hz;
		client->SendLooped((uint8_t*)&samplerate_fs, sizeof(samplerate_fs));

		double delta_s = ((double)(get_ms() - g_session_start_ms)) / 1000;

		if ((delta_s - g_lastReportedRate) > 10) {
			g_lastReportedRate = delta_s;

			double wfms_s = hwSeqnum / delta_s;
			LogDebug("WaveformServerThread/bus: HWSeq#%u: %lu samples on %d channels, HW WFMs/s=%f\n", hwSeqnum, num_samples, numchans, wfms_s);
		}

		chindex = 0;
		for (size_t chnum : sample_channels) {
			//Send channel ID, memory depth
			client->SendLooped((uint8_t*)&chnum, sizeof(chnum));
			client->SendLooped((uint8_t*)&num_samples, sizeof(num_samples));

			struct sr_channel* ch = g_channels[chnum];

			if (g_deviceIsScope) {
				float scale, offset;
				compute_scale_and_offset(ch, scale, offset);

				float config[3] = {scale, offset, trigphase};
				client->SendLooped((uint8_t*)&config, sizeof(config));

				bool ch_clipping = clipping[chindex];
				client->SendLooped((uint8_t*)&ch_clipping, sizeof(ch_clipping));
			} else {
				client->SendLooped((uint8_t*)&first_sample, sizeof(first_sample));
			}

			//Send the actual waveform data
			client->SendLooped(deinterleaved_buffers[chindex], num_samples * sizeof(int8_t));

			chindex++;
		}

		for (auto i : deinterleaved_buffers) {
			delete[] i;
		}

		if (g_oneShot) {
			LogDebug("Stopping after oneshot\n");
			g_run = false;
			sr_session_stop();
		}
	}
}

void readAckThread(Socket* client) {
	for (;;) {
		int64_t state[2];

		if (!client->RecvLooped((uint8_t*)&state, sizeof(state))) {
			LogError("Failed to receive ACK; disconnected.\n");
			std::terminate();
		}

		g_windowSize = state[1];
		g_lastAcked = state[0];
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

	std::thread dataThread(readAckThread, &client);

	for (;;) {
		if (g_quit) {
			return;
		} else if (!g_run) {
			usleep(100);
			continue;
		}

		// LogDebug("Starting Session...\n");

		g_running = true;
		g_capturedFirstFrame = false;

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

		// LogDebug("Session Stopped.\n");
	}
}
