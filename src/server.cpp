
#include "server.h"

#include "log/log.h"
#include "xptools/Socket.h"

#include "srbinding.h"

#include <string>
#include <vector>
#include <thread>
#include <cassert>

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

volatile bool g_waveformThreadQuit = false;

std::vector<struct sr_channel*> g_channels;
std::vector<uint64_t> g_vdiv_options;
int g_numdivs;

void populate_vdivs(const struct sr_dev_inst *sdi, std::vector<uint64_t>& vec) {
	GVariant *gvar_list, *gvar_list_vdivs;
    if (sr_config_list(sdi->driver, sdi,
                       NULL, SR_CONF_PROBE_VDIV, &gvar_list) == SR_OK) {
        assert(gvar_list);
        if ((gvar_list_vdivs = g_variant_lookup_value(gvar_list,
                "vdivs", G_VARIANT_TYPE("at")))) {
            GVariant *gvar;
            GVariantIter iter;
            g_variant_iter_init(&iter, gvar_list_vdivs);
            while(NULL != (gvar = g_variant_iter_next_value(&iter))) {
                vec.push_back(g_variant_get_uint64(gvar));
                g_variant_unref(gvar);
            }
            g_variant_unref(gvar_list_vdivs);
            g_variant_unref(gvar_list);
        }
    }
}

void run_server(struct sr_dev_inst *device, int scpi_port) {
	for (GSList *l = device->channels; l; l = l->next) {
        struct sr_channel* ch = (struct sr_channel*)l->data;
        g_channels.push_back(ch);
    }

    g_numdivs = DS_CONF_DSO_VDIVS;
	// TODO: SR_CONF_NUM_VDIV instead of DS_CONF_DSO_VDIVS on regular sigrok

	populate_vdivs(device, g_vdiv_options);

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

			// LogDebug("Receive: %s\n", line.c_str());

			string subject, cmd;
			bool query;
			vector<string> args;
			ParseScpiLine(line, subject, cmd, query, args);

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
				else
					LogWarning("Unknown or malformed query: %s\n", line.c_str());
			}
			else
			{
				if (cmd == "COUP" && subj_ch && args.size() == 1) {
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
				} else if (cmd == "RANGE" && subj_ch && args.size() == 1) {
					float range_V = stod(args[0]);
					float range_mV_per_div = (range_V / g_numdivs) * 1000;

					uint64_t selected = g_vdiv_options[g_vdiv_options.size()];

					for (auto i : g_vdiv_options) {
						if ((i > selected) && !(i > range_mV_per_div)) {
							selected = i;
						}
					}

					set_probe_config<uint64_t>(device, subj_ch, SR_CONF_PROBE_VDIV, selected);
					// LogDebug("Wanted %f, result: %lu\n", range_mV_per_div, selected);

				} else if (cmd == "RATE" && subj_dev && args.size() == 1) {
					set_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE, stoull(args[0]));
				} else if (cmd == "DEPTH" && subj_dev && args.size() == 1) {
					set_dev_config<uint64_t>(device, SR_CONF_LIMIT_SAMPLES, stoull(args[0]));
				} else {
					LogWarning("Unknown or malformed command: %s\n", line.c_str());
				}
			}
		}
	}
}

void waveform_callback (const struct sr_dev_inst *device, const struct sr_datafeed_packet *packet, void* client_vp) {
	Socket* client = (Socket*) client_vp;

	if (packet->type == SR_DF_DSO) {
		struct sr_datafeed_dso* dso = (struct sr_datafeed_dso*)packet->payload;
		size_t num_samples = dso->num_samples;

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

		for (int p = 0; p < num_samples; p++) {
			int8_t* pp = (int8_t*)(buf + p);

			*pp = *(buf + p) - 127;

			// TODO: This is slow and can't be the best way
		}

		float vdiv_mV = get_probe_config<uint64_t>(device, ch, SR_CONF_PROBE_VDIV).value();
		float hwmin = get_dev_config<uint32_t>(device, SR_CONF_REF_MIN).value_or(0);
		float hwmax = get_dev_config<uint32_t>(device, SR_CONF_REF_MAX).value_or((1 << numbits) - 1);
		float full_throw_V = vdiv_mV / 1000 * g_numdivs; // Volts indicated by most-positive value (255)
		float hwrange_factor = (255.f / (hwmax - hwmin));
		float scale = 1 / 255.f * full_throw_V;
		float offset = 0;//127 * scale; // hwmin?

		// (X - 127) * S -> X * S - 127 * S

		int8_t* p = (int8_t*)(buf + 100);
		int8_t* p2 = (int8_t*)(buf + 5100);
		float v = *p;//(int)((*p) - 127);
		float vc = *p2;
		LogDebug("vdiv_mV = %f, ft_V=%f, scale=%f, off=%f\n", vdiv_mV, full_throw_V, scale, offset);
		LogDebug(" ... v=%d -> %f   ;; vc=%d -> %f\n", (int)v, v*scale - offset, (int)vc, vc*scale - offset);

		float trigphase = 0;

		float config[3] = {scale, offset, trigphase};
		client->SendLooped((uint8_t*)&config, sizeof(config));

		//Send the actual waveform data
		client->SendLooped((uint8_t*)buf, num_samples * sizeof(int8_t));
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

	int err;
	if ((err = sr_session_start()) != SR_OK) {
		LogError("session_start returned failure: %d\n", err);
		return;
	}
	if ((err = sr_session_run()) != SR_OK) {
		LogError("session_run returned failure: %d\n", err);
		return;
	}
}