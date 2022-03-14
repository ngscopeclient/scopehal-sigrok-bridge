
#include "server.h"

#include "log/log.h"
#include "xptools/Socket.h"

#include "srbinding.h"

#include <string>
#include <vector>
#include <thread>
#include <cassert>
#include <semaphore>

#include <stdlib.h>

using std::string;
using std::vector;
using std::to_string;
using std::thread;

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

vector<struct sr_channel*> g_channels;
int g_numdivs;

bool g_runAcq;
bool g_triggerOneShot = false;

void run_server(struct sr_dev_inst *device, int scpi_port) {
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

	uint64_t curr_bufsz = get_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES).value();

	// Driver does not report discrete steps; make some up
	string depths_string;
	int step = 250000;
	for (uint64_t i = step; i <= curr_bufsz; i += step) {
		depths_string += to_string(i) + ",";
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
						if (!state && subj_ch == g_channels[0]) {
							LogWarning("Ignoring request to disable ch0\n");
						} else {
							set_probe_config<bool>(device, subj_ch, SR_CONF_PROBE_EN, state);
							LogDebug("Updated enabled for ch%s, now %s\n", subject.c_str(), cmd.c_str());

							if (g_runAcq) {
								sr_session_stop(); // Restart acquisition
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
					if (cmd == "DELAY") {
						
					} else if (cmd == "SOU") {
						
					} else if (cmd == "LEV" && args.size()==1) {
						double level = stod(args[0]);
					} else if (cmd == "EDGE:DIR") {

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

						g_runAcq = true;
					} else if (cmd == "STOP" && args.size() == 0) {
						LogDebug("Stopping...\n");
						g_runAcq = 0;
						sr_session_stop();
					} else {
						LogWarning("Unknown command: %s\n", line.c_str());
					}
				}
			}
		}
	}
}


// float InterpolateTriggerTime(int8_t* buf)
// {
// 	if(g_triggerSampleIndex <= 0)
// 		return 0;

// 	float trigscale = g_roundedRange[g_triggerChannel] / 32512;
// 	float trigoff = g_offsetDuringArm[g_triggerChannel];

// 	float fa = buf[g_triggerSampleIndex-1] * trigscale + trigoff;
// 	float fb = buf[g_triggerSampleIndex] * trigscale + trigoff;

// 	//no need to divide by time, sample spacing is normalized to 1 timebase unit
// 	float slope = (fb - fa);
// 	float delta = g_triggerVoltage - fa;
// 	return delta / slope;
// }


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

            LogNotice("Trigger packet; real_pos=%d\n", trig_pos);
		}

	} else if (packet->type == SR_DF_DSO) {
		if (!g_runAcq) return;
		// Don't send further data packets after stop requested

		struct sr_datafeed_dso* dso = (struct sr_datafeed_dso*)packet->payload;
		size_t num_samples = dso->num_samples;

		LogDebug("Yield samples: %lu\n", num_samples);

		uint8_t* buf = (uint8_t*) dso->data;

		uint16_t numchans = 1;
		client->SendLooped((uint8_t*)&numchans, sizeof(numchans));

		uint64_t samplerate_hz = get_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE).value();

		int64_t samplerate_fs = 1000000000000000 / samplerate_hz;
		client->SendLooped((uint8_t*)&samplerate_fs, sizeof(samplerate_fs));

		//Send channel ID, scale, offset, and memory depth
		size_t chnum = 0;
		client->SendLooped((uint8_t*)&chnum, sizeof(chnum));
		client->SendLooped((uint8_t*)&num_samples, sizeof(num_samples));

		struct sr_channel* ch = g_channels[chnum];

		uint8_t numbits = get_dev_config<uint8_t>(device, SR_CONF_UNIT_BITS).value();

		assert(numbits == 8);

		float vdiv_mV = get_probe_config<uint64_t>(device, ch, SR_CONF_PROBE_VDIV).value();
		float hwmin = get_dev_config<uint32_t>(device, SR_CONF_REF_MIN).value_or(0);
		float hwmax = get_dev_config<uint32_t>(device, SR_CONF_REF_MAX).value_or((1 << numbits) - 1);
		float full_throw_V = vdiv_mV / 1000 * g_numdivs;  // Volts indicated by most-positive value (255)
		float hwrange_factor = (255.f / (hwmax - hwmin)); // Adjust for incomplete range of ADC reports
		float scale = hwrange_factor / 255.f * full_throw_V;
		float offset = 127 * scale; // Zero is 127

		float trigphase = 0;

		float config[3] = {scale, offset, trigphase};
		client->SendLooped((uint8_t*)&config, sizeof(config));

		//Send the actual waveform data
		client->SendLooped((uint8_t*)buf, num_samples * sizeof(int8_t));

		if (g_triggerOneShot) {
			LogDebug("Stopping after oneshot\n");
			g_runAcq = false;
			sr_session_stop();
		}
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
		if ((err = sr_session_run()) != SR_OK) {
			LogError("session_run returned failure: %d\n", err);
			return;
		}

		LogDebug("Session Stopped.\n");
	}
}