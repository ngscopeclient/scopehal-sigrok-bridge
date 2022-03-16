
#include "server.h"

#include "log/log.h"
#include "xptools/Socket.h"

#include "srbinding.h"

#include <string>
#include <vector>
#include <thread>
#include <cassert>
#include <semaphore>
#include <utility>

#include <stdlib.h>

using std::string;
using std::vector;
using std::to_string;
using std::thread;
using std::pair;

Socket g_scpiSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
Socket g_dataSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

void WaveformServerThread();

bool ScpiSend(Socket& sock, const string& cmd)
{
	string tempbuf = cmd + "\n";

	// LogDebug("Send: %s", tempbuf.c_str());
	return sock.SendLooped((unsigned char*)tempbuf.c_str(), tempbuf.length());
}

bool ScpiRecv(Socket& sock, string& str)
{
	int sockid = sock;

	char tmp = ' ';
	str = "";
	while(true)
	{
		if(1 != recv(sockid, &tmp, 1, MSG_WAITALL))
			return false;

		if( (tmp == '\n') || ( (tmp == ';') ) )
			break;
		else
			str += tmp;
	}

	return true;
}

void ParseScpiLine(const string& line, string& subject, string& cmd, bool& query, vector<string>& args)
{
	//Reset fields
	query = false;
	subject = "";
	cmd = "";
	args.clear();

	string tmp;
	bool reading_cmd = true;
	for(size_t i=0; i<line.length(); i++)
	{
		//If there's no colon in the command, the first block is the command.
		//If there is one, the first block is the subject and the second is the command.
		//If more than one, treat it as freeform text in the command.
		if( (line[i] == ':') && subject.empty() )
		{
			subject = tmp;
			tmp = "";
			continue;
		}

		//Detect queries
		if(line[i] == '?')
		{
			query = true;
			continue;
		}

		//Comma delimits arguments, space delimits command-to-args
		if(!(isspace(line[i]) && cmd.empty()) && line[i] != ',')
		{
			tmp += line[i];
			continue;
		}

		//merge multiple delimiters into one delimiter
		if(tmp == "")
			continue;

		//Save command or argument
		if(reading_cmd)
			cmd = tmp;
		else
			args.push_back(tmp);

		reading_cmd = false;
		tmp = "";
	}

	//Stuff left over at the end? Figure out which field it belongs in
	if(tmp != "")
	{
		if(cmd != "")
			args.push_back(tmp);
		else
			cmd = tmp;
	}
}

struct sr_dev_inst* g_device;
vector<struct sr_channel*> g_channels;
int g_numdivs;
int g_selectedTriggerChannel;

bool g_runAcq = false;
bool g_acqRunning = false;
bool g_triggerOneShot = false;

void compute_scale_and_offset(struct sr_channel* ch, float& scale, float& offset) {
	float vdiv_mV = get_probe_config<uint64_t>(g_device, ch, SR_CONF_PROBE_VDIV).value();
	float hwmin = get_dev_config<uint32_t>(g_device, SR_CONF_REF_MIN).value_or(0);
	float hwmax = get_dev_config<uint32_t>(g_device, SR_CONF_REF_MAX).value_or((1 << 8) - 1);
	float full_throw_V = vdiv_mV / 1000 * g_numdivs;  // Volts indicated by most-positive value (255)
	float hwrange_factor = (255.f / (hwmax - hwmin)); // Adjust for incomplete range of ADC reports
	scale = -1 * hwrange_factor / 255.f * full_throw_V;
	offset = 127 * scale; // Zero is 127
}

int count_enabled_channels() {
	int enabled = 0;

	for (auto ch : g_channels) {
		if (get_probe_config<bool>(g_device, ch, SR_CONF_PROBE_EN)) {
			enabled++;
		}
	}

	return enabled;
}

void run_server(struct sr_dev_inst *device, int scpi_port) {
	g_device = device;

	for (GSList *l = device->channels; l; l = l->next) {
        struct sr_channel* ch = (struct sr_channel*)l->data;
        g_channels.push_back(ch);
    }

    g_numdivs = DS_CONF_DSO_VDIVS;
	// TODO: SR_CONF_NUM_VDIV instead of DS_CONF_DSO_VDIVS on regular sigrok

	// set_dev_config<bool>(device, SR_CONF_STREAM, true);
	// TODO: What does this do? Do we care?

	vector<uint64_t> vdiv_options = get_dev_config_options<uint64_t>(device, SR_CONF_PROBE_VDIV);
	vector<uint64_t> rate_options = get_dev_config_options<uint64_t>(device, SR_CONF_SAMPLERATE);

	string samplerates_string;
	for (auto i : rate_options) {
		samplerates_string += to_string(i) + ",";
	}

	set_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES, 10000000);
	set_dev_config<uint8_t>(device, SR_CONF_TRIGGER_SOURCE, DSO_TRIGGER_CH0);
	g_selectedTriggerChannel = 0;

	// uint64_t curr_bufsz = get_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES).value();

	// 10000000 per probe when both active, 2*10000000 when one active
	// Driver does not report discrete steps; make some up

	vector<uint64_t> depths {
		10,
		100,
		1000,
		10000,
		100000,
	};

	vector<uint64_t> muls {
		10,
		25,
		50,
	};

	string depths_string;
	for (auto d : depths) {
		for (auto m : muls) {
			depths_string += to_string(d*m) + ",";
		}
	}

	LogDebug("Report available sample rates: %s\n", samplerates_string.c_str());
	LogDebug("Report available buffer depths: %s\n", depths_string.c_str());

	//Launch the control plane socket server
	g_scpiSocket.Bind(scpi_port);
	g_scpiSocket.Listen();

	//Configure the data plane socket
	g_dataSocket.Bind(scpi_port + 1);
	g_dataSocket.Listen();

	LogNotice("Started SCPI server at %d\n", scpi_port);

	while(true)
	{
		Socket client = g_scpiSocket.Accept();
		Socket dataClient(-1);
		LogVerbose("Client connected to control plane socket\n");

		if(!client.IsValid())
			break;
		if(!client.DisableNagle())
			LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

		thread dataThread(WaveformServerThread);

		//Main command loop
		while(true)
		{
			string line;
			if(!ScpiRecv(client, line))
				break;

			string subject, cmd;
			bool query;
			vector<string> args;
			ParseScpiLine(line, subject, cmd, query, args);

			// LogDebug("Receive: %s\n", line.c_str());
			// LogDebug("Parsed: s='%s', c='%s', q=%d, a=[", subject.c_str(), cmd.c_str(), query);
			// for (auto i : args) {
			// 	LogDebug("'%s',", i.c_str());
			// }
			// LogDebug("]\n");

			bool subj_dev = false;
			bool subj_trig = false;
			struct sr_channel* subj_ch = NULL;
			if (subject.length() == 1 && isdigit(subject[0])) {
				int index = subject[0] - '0';
				if (index < (int)g_channels.size()) {
					subj_ch = g_channels[index];
				} else {
					LogWarning("Invalid channel subject: %s\n", subject.c_str());
				}
			} else if (subject.length() == 0) {
				subj_dev = true;
			} else {
				subj_trig = subject == "TRIG";

				if (!subj_trig)
					LogWarning("Unknown subject: %s\n", subject.c_str());
			}

			if(query)
			{
				//Read ID code
				if(cmd == "*IDN")
					ScpiSend(client, string("DreamSourceLabs (bridge),") + device->model + ",NOSERIAL,NOVERSION");
				else if (cmd == "CHANS")
					ScpiSend(client, to_string(2));
				else if (cmd == "RATES")
					ScpiSend(client, samplerates_string);
				else if (cmd == "DEPTHS")
					ScpiSend(client, depths_string);
				else
					LogWarning("Unknown or malformed query: %s\n", line.c_str());
			}
			else
			{
				// Channel commands
				if (subj_ch) {
					if ((cmd == "ON" || cmd == "OFF") && args.size() == 0) {
						bool state = cmd == "ON";
						if (!state && subj_ch == g_channels[0] && !get_probe_config<bool>(device, g_channels[1], SR_CONF_PROBE_EN)) {
							LogWarning("Ignoring request to disable ch0 because it would disable all channels\n");
						} else {
							// Must stop acquisition while disabling probe or we crash inside vendor code
							bool wasRunning = g_acqRunning;
							if (wasRunning) {
								g_runAcq = false;
								sr_session_stop();

								// Block until stopped
								while (g_acqRunning) usleep(100);
							}

							set_probe_config<bool>(device, subj_ch, SR_CONF_PROBE_EN, state);
							LogDebug("Updated ENABLED for ch%s, now %s\n", subject.c_str(), cmd.c_str());
							
							// If we were running, restart
							if (wasRunning) {
								g_runAcq = true;
							}
						}
					} else if (cmd == "COUP" && args.size() == 1) {
						string coup_str = args[0];
						uint8_t sr_coupling = -1;

						if (coup_str == "AC1M") {
							sr_coupling = SR_AC_COUPLING;
						} else if (coup_str == "DC1M") {
							sr_coupling = SR_DC_COUPLING;
						} else {
							LogWarning("Unknown coupling: %s\n", coup_str.c_str());
							continue;
						}

						set_probe_config<uint8_t>(device, subj_ch, SR_CONF_PROBE_COUPLING, sr_coupling);
						LogDebug("Updated coupling for ch%s, now %s\n", subject.c_str(), coup_str.c_str());
					} else if (cmd == "RANGE" && args.size() == 1) {
						float range_V = stod(args[0]);
						float range_mV_per_div = (range_V / g_numdivs) * 1000;

						uint64_t selected = vdiv_options[vdiv_options.size()];

						for (auto i : vdiv_options) {
							if ((i > selected) && !(i > range_mV_per_div)) {
								selected = i;
							}
						}

						set_probe_config<uint64_t>(device, subj_ch, SR_CONF_PROBE_VDIV, selected);
						LogDebug("Updated RANGE; Wanted %f, result: %lu\n", range_mV_per_div, selected);
					} else if (cmd == "OFFS" && args.size() == 1) {
						LogDebug("Ignored OFFS\n");
					} else {
						LogWarning("Unknown command: %s\n", line.c_str());
					}
				}

				// Trigger commands
				if (subj_trig) {
					if (cmd == "DELAY" && args.size() == 1) {
						LogDebug("DELAY request: %s\n", line.c_str());
						uint64_t delay = stoull(args[0]);

						int numchans = count_enabled_channels();

						uint64_t samples_in_full_capture = get_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES).value();
						uint64_t samplerate_hz = get_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE).value();

						if (samplerate_hz == 1000000000 && numchans == 2) {
							// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
							samplerate_hz /= 2;
						}

						uint64_t fs_per_sample = 1000000000000000 / samplerate_hz;
						uint64_t fs_in_full_capture = samples_in_full_capture * fs_per_sample;
						double pct = ((double)delay / (double)fs_in_full_capture) * (double)100;
						uint8_t result = pct;

						LogWarning("samples=%lu, hz=%lu, fsper=%lu, fsinfull=%lu, pct=%f\n", 
							samples_in_full_capture, samplerate_hz, fs_per_sample, fs_in_full_capture, pct);

						bool wasRunning = g_acqRunning;
						if (wasRunning) {
							g_runAcq = false;
							sr_session_stop();

							// Block until stopped
							while (g_acqRunning) usleep(100);
						}

						set_dev_config<uint8_t>(device, SR_CONF_HORIZ_TRIGGERPOS, result);

						// If we were running, restart
						if (wasRunning) {
							g_runAcq = true;
						}

						LogDebug("Set trigger DELAY to %lu (%%%d)\n", delay, result);
					} else if (cmd == "SOU" && args.size() == 1) {
						int channel = args[0][0] - '0';

						int sr_channel = -1;

						if (channel == 0) {
							sr_channel = DSO_TRIGGER_CH0;
						} else if (channel == 1) {
							sr_channel = DSO_TRIGGER_CH1;
						} else {
							LogWarning("Unknown trigger source: %s\n", args[0].c_str());
							break;
						}

						set_dev_config<uint8_t>(device, SR_CONF_TRIGGER_SOURCE, sr_channel);

						g_selectedTriggerChannel = channel;

						LogDebug("Set trigger SOU to %d\n", channel);
					} else if (cmd == "LEV" && args.size() == 1) {
						double level = stod(args[0]);

						// Set it on all probes, allowing SR_CONF_TRIGGER_SOURCE to select
						// which is actually active
						for (auto ch : g_channels) {
							float scale, offset;
							compute_scale_and_offset(ch, scale, offset);

							// voltage = ADC * scale - offset
							// ADC = (voltage + offset) / scale

							uint8_t adc = (level + offset) / scale;

							LogDebug("Setting LEV on ch%d; adc=%d\n", ch->index, adc);

							set_probe_config<uint8_t>(device, ch, SR_CONF_TRIGGER_VALUE, adc);
						}

						LogDebug("Set trigger LEV to %f\n", level);
					} else if (cmd == "EDGE:DIR" && args.size() == 1) {
						string edge = args[0];
						int sr_edge = -1;

						if (edge == "RISING") {
							sr_edge = DSO_TRIGGER_RISING;
						} else if (edge == "FALLING") {
							sr_edge = DSO_TRIGGER_FALLING;
						} else {
							LogWarning("Unsupported trigger EDGE: %s\n", edge.c_str());
							break;
						}

						set_dev_config<uint8_t>(device, SR_CONF_TRIGGER_SLOPE, sr_edge);

						LogDebug("Set trigger EDGE to %s\n", edge.c_str());
					} else {
						LogWarning("Unknown command: %s\n", line.c_str());
					}
				}

				// Device commands
				if (subj_dev) {
					if (cmd == "RATE" && args.size() == 1) {
						uint64_t rate = stoull(args[0]);
						set_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE, rate);
						LogDebug("Updated RATE; now %lu\n", rate);
					} else if (cmd == "DEPTH" && args.size() == 1) {
						uint64_t depth = stoull(args[0]);
						set_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES, depth);
						LogDebug("Updated DEPTH; now %lu\n", depth);
					} else if ((cmd == "START" || cmd == "SINGLE" || cmd == "FORCE") && args.size() == 0) {
						LogDebug("Starting %s...\n", cmd.c_str());
						g_triggerOneShot = !(cmd == "START");

						// set_dev_config<bool>(device, SR_CONF_INSTANT, cmd == "FORCE");
						// This appears in the DSView source, but setting it on the DSO seems to make it
						// never trigger

						// Actually stopping and starting acquisition for SINGLE is noticeably slow (~0.2s);
						// Maybe just leave running and send single frames when SINGLE requested?

						g_runAcq = false;
						if (g_acqRunning) sr_session_stop();
						while (g_acqRunning) usleep(100);

						g_runAcq = true;
					} else if (cmd == "STOP" && args.size() == 0) {
						LogDebug("Stopping...\n");
						g_runAcq = false;
						sr_session_stop();
					} else {
						LogWarning("Unknown command: %s\n", line.c_str());
					}
				}
			}
		}
	}
}

uint32_t g_lastTrigPos = 0;

float InterpolateTriggerTime(struct sr_channel *ch, uint8_t* buf, uint32_t trigpos)
{
	// if(g_triggerSampleIndex <= 0)
	// 	return 0;

	// float trigscale = g_roundedRange[g_triggerChannel] / 32512;
	// float trigoff = g_offsetDuringArm[g_triggerChannel];

	// float fa = buf[g_triggerSampleIndex-1] * trigscale + trigoff;
	// float fb = buf[g_triggerSampleIndex] * trigscale + trigoff;

	// //no need to divide by time, sample spacing is normalized to 1 timebase unit
	// float slope = (fb - fa);
	// float delta = g_triggerVoltage - fa;
	// return delta / slope;

	if (trigpos <= 0) {
		return 0;
	}

	// These are all already in ADC values, so no need to scale
	uint8_t trigvalue = ch->trig_value;

	uint8_t pretrig = buf[trigpos-1];
	uint8_t afttrig = buf[trigpos];
	float slope = afttrig - pretrig;
	float delta = trigvalue - pretrig;
	float phase = (delta / slope);
	float final = - ( 1 - phase ); // ADC values are 'upside down'

	if (phase < 0 || phase > 1) {
		LogWarning("Something has gone wrong in trigphase (phase=%f)\n", phase);
		LogDebug("TP=%d, TV=%d, A=%d, B=%d, slope=%f, delta=%f, final=%f\n", trigpos, trigvalue, pretrig, afttrig, slope, delta, final);

		for (int i = -50; i < 50; i++) {
			printf("buf[trigpos%+d] = %03d, ", i, buf[trigpos-i]);
		}

		printf("\n");

		return 0;
	}

	return final;
}

uint32_t g_seqnum = 0;
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

		if (trigger->status & 0x01) {
            uint32_t trig_pos = trigger->real_pos;

            g_lastTrigPos = trig_pos * 2 / count_enabled_channels();

            LogWarning("Trigger packet; real_pos=%d, g_lastTrigPos=%d\n", trig_pos, g_lastTrigPos);
		}

	} else if (packet->type == SR_DF_DSO) {
		if (!g_runAcq) {
			LogWarning("SR_DF_DSO: !g_runAcq; ignoring\n");
			return;
		}
		// Don't send further data packets after stop requested

		if (!g_pendingAcquisition) {
			// LogWarning("SR_DF_DSO: !g_pendingAcquisition; ignoring to avoid buffering\n");
			return;
		}

		g_pendingAcquisition = false;

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

		// for (GSList *l = dso->probes; l != NULL; l = l->next) {
  //       	sample_channels.push_back(((struct sr_channel*)l->data)->index);
  //       }

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

		float trigphase = InterpolateTriggerTime(g_channels[g_selectedTriggerChannel], buffer_for_trigphase, g_lastTrigPos);
		// trigphase needs to come from the channel that the trigger is on for all channels.
		// TODO: does this mean we need to offset the other channel by samplerate_fs/2 though if the
		// ADC sample is 180deg out of phase?

		uint32_t seqnum = g_seqnum++;

		client->SendLooped((uint8_t*)&seqnum, sizeof(seqnum));

		client->SendLooped((uint8_t*)&numchans, sizeof(numchans));

		// TODO: This is maybe wrong? (the / numchans)
		uint64_t samplerate_hz = get_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE).value();

		if (samplerate_hz == 1000000000 && numchans == 2) {
			// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
			samplerate_hz /= 2;
		}

		int64_t samplerate_fs = 1000000000000000 / samplerate_hz;
		client->SendLooped((uint8_t*)&samplerate_fs, sizeof(samplerate_fs));

		// LogDebug("SR_DF_DSO: Seq#%u: %lu samples on %d channels\n", seqnum, num_samples, numchans);

		int chindex = 0;
		for (size_t chnum : sample_channels) {
			//Send channel ID, scale, offset, and memory depth
			client->SendLooped((uint8_t*)&chnum, sizeof(chnum));
			client->SendLooped((uint8_t*)&num_samples, sizeof(num_samples));

			struct sr_channel* ch = g_channels[chnum];

			uint8_t numbits = get_dev_config<uint8_t>(device, SR_CONF_UNIT_BITS).value();

			assert(numbits == 8);

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

		if (g_triggerOneShot) {
			LogDebug("Stopping after oneshot\n");
			g_runAcq = false;
			sr_session_stop();
		}
	}
}

void syncWaitThread(Socket* client) {
	for (;;) {
		uint8_t r = '0';
		client->RecvLooped(&r, 1);
		if (r != 'K') {
			LogWarning("syncWaitThread: Failed to get go-ahead from scopehal, ignoring\n");
			continue;
		}

		g_pendingAcquisition = true;
	}
}

void WaveformServerThread()
{
	Socket client = g_dataSocket.Accept();
	LogVerbose("Client connected to data plane socket\n");

	if(!client.IsValid())
		return;
	if(!client.DisableNagle())
		LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

	sr_session_datafeed_callback_add(waveform_callback, &client);

	thread dataThread(syncWaitThread, &client);

	for (;;) {
		while (!g_runAcq) {
			usleep(1000); //1ms
		}

		LogDebug("Starting Session...\n");

		int err;
		if ((err = sr_session_start()) != SR_OK) {
			LogError("session_start returned failure: %d\n", err);
			return;
		}

		g_acqRunning = true;

		if ((err = sr_session_run()) != SR_OK) {
			LogError("session_run returned failure: %d\n", err);
			return;
		}

		LogDebug("Session Stopped.\n");
		g_acqRunning = false;
	}
}