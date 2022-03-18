
#include "server.h"
#include "SigrokSCPIServer.h"
#include "srbinding.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SigrokSCPIServer::SigrokSCPIServer(ZSOCKET sock)
	: BridgeSCPIServer(sock)
{
	;
}

SigrokSCPIServer::~SigrokSCPIServer()
{
	LogVerbose("Client disconnected\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command parsing

size_t SigrokSCPIServer::GetChannelID(const string& subject)
{
	return subject[0] - '0';
}

bool SigrokSCPIServer::OnQuery(
		const string& line,
		const string& subject,
		const string& cmd)
{
	if(BridgeSCPIServer::OnQuery(line, subject, cmd))
		return true;

	//TODO: handle commands not implemented by the base class
	LogWarning("Unrecognized query received: %s\n", line.c_str());

	return false;
}

string SigrokSCPIServer::GetMake()
{
	return "DreamSourceLabs (bridge)";
}

string SigrokSCPIServer::GetModel()
{
	return g_sr_device->model;
}

string SigrokSCPIServer::GetSerial()
{
	return "NOSERIAL";
}

string SigrokSCPIServer::GetFirmwareVersion()
{
	return "NOVERSION";
}

size_t SigrokSCPIServer::GetAnalogChannelCount()
{
	return g_channels.size();
}

vector<size_t> SigrokSCPIServer::GetSampleRates()
{
	vector<uint64_t> rate_options = get_dev_config_options<uint64_t>(g_sr_device, SR_CONF_SAMPLERATE);

	return rate_options;
}

vector<size_t> SigrokSCPIServer::GetSampleDepths()
{
	vector<size_t> depths {
		10,
		100,
		1000,
		10000,
		100000,
	};

	vector<size_t> muls {
		10,
		25,
		50,
	};

	vector<size_t> result;
	for (auto d : depths) {
		for (auto m : muls) {
			result.push_back(d*m);
		}
	}

	return result;
}

void SigrokSCPIServer::OnCommand(
		const string& line,
		const string& subject,
		const string& cmd,
		const std::vector<std::string>& args)
{
	(void) line;
	
	if (cmd == "START" || cmd == "SINGLE" || cmd == "FORCE") {
		LogDebug("cmd: START\n");

		g_oneShot = cmd != "START";
		g_run = true;

		force_correct_sample_config();
	} else if (cmd == "STOP") {
		LogDebug("cmd: STOP\n");

		g_run = false;
		sr_session_stop();
	} else if (cmd == "ON" || cmd == "OFF") {
		bool state = cmd == "ON";
		if (!state && GetChannelID(subject) == 0 && !get_probe_config<bool>(g_sr_device, g_channels[1], SR_CONF_PROBE_EN)) {
			LogWarning("Ignoring request to disable ch0 because it would disable all channels\n");
		} else {
			// Must stop acquisition while disabling probe or we crash inside vendor code
			bool wasRunning = stop_capture_sync();

			set_probe_config<bool>(g_sr_device, g_channels[GetChannelID(subject)], SR_CONF_PROBE_EN, state);
			LogDebug("Updated ENABLED for ch%s, now %s\n", subject.c_str(), cmd.c_str());
			
			if (wasRunning) restart_capture();
		}
	} else if (cmd == "COUP") {
		string coup_str = args[0];
		uint8_t sr_coupling = -1;

		if (coup_str == "AC1M") {
			sr_coupling = SR_AC_COUPLING;
		} else if (coup_str == "DC1M") {
			sr_coupling = SR_DC_COUPLING;
		} else {
			LogWarning("Unknown coupling: %s\n", coup_str.c_str());
			return;
		}

		set_probe_config<uint8_t>(g_sr_device, g_channels[GetChannelID(subject)], SR_CONF_PROBE_COUPLING, sr_coupling);
		LogDebug("Updated coupling for ch%s, now %s\n", subject.c_str(), coup_str.c_str());
	} else if (cmd == "RANGE") {
		float range_V = stod(args[0]);
		float range_mV_per_div = (range_V / g_numdivs) * 1000;

		uint64_t selected = vdiv_options[vdiv_options.size()];

		for (auto i : vdiv_options) {
			if ((i > selected) && !(i > range_mV_per_div)) {
				selected = i;
			}
		}

		set_probe_config<uint64_t>(g_sr_device, g_channels[GetChannelID(subject)], SR_CONF_PROBE_VDIV, selected);
		LogDebug("Updated RANGE; Wanted %f, result: %lu\n", range_mV_per_div, selected);
	} else if (cmd == "RATE") {
		set_rate(stoull(args[0]));

		LogDebug("Updated RATE; now %lu\n", g_rate);
	} else if (cmd == "DEPTH") {
		set_depth(stoull(args[0]));

		LogDebug("Updated DEPTH; now %lu\n", g_depth);
	} else if (subject == "TRIG") {
		if (cmd == "DELAY" && args.size() == 1) {
			LogDebug("DELAY request: %s\n", line.c_str());
			uint64_t delay = stoull(args[0]);

			int numchans = count_enabled_channels();

			uint64_t samples_in_full_capture = get_dev_config<uint64_t>(g_sr_device, SR_CONF_LIMIT_SAMPLES).value();
			uint64_t samplerate_hz = get_dev_config<uint64_t>(g_sr_device, SR_CONF_SAMPLERATE).value();

			if (samplerate_hz == 1000000000 && numchans == 2) {
				// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
				samplerate_hz /= 2;
			}

			uint64_t fs_per_sample = 1000000000000000 / samplerate_hz;
			uint64_t fs_in_full_capture = samples_in_full_capture * fs_per_sample;
			double pct = ((double)delay / (double)fs_in_full_capture) * (double)100;
			uint8_t result = pct;

			// LogWarning("samples=%lu, hz=%lu, fsper=%lu, fsinfull=%lu, pct=%f\n", 
				// samples_in_full_capture, samplerate_hz, fs_per_sample, fs_in_full_capture, pct);

			set_dev_config<uint8_t>(g_sr_device, SR_CONF_HORIZ_TRIGGERPOS, result);

			LogDebug("Set trigger DELAY to %lu (%%%d)\n", delay, result);
		} else if (cmd == "SOU" && args.size() == 1) {
			int channel = args[0][0] - '0';

			set_trigger_channel(channel);

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

				set_probe_config<uint8_t>(g_sr_device, ch, SR_CONF_TRIGGER_VALUE, adc);
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
				return;
			}

			set_dev_config<uint8_t>(g_sr_device, SR_CONF_TRIGGER_SLOPE, sr_edge);

			LogDebug("Set trigger EDGE to %s\n", edge.c_str());
		}
	}
}

