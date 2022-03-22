
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

bool SigrokSCPIServer::GetChannelID(const std::string& subject, size_t& id_out)
{
	int result;

	if (subject.size() == 1) {
		result = subject[0] - '0';
	} else {
		result = 10 + (subject[1] - '0');
	}

	if (result < 0 || result > (int)GetAnalogChannelCount()) {
		return false;
	}

	id_out = result;
	return true;
}

BridgeSCPIServer::ChannelType SigrokSCPIServer::GetChannelType(size_t channel)
{
	if (g_deviceIsScope)
		return CH_ANALOG;
	else
		return CH_DIGITAL;
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

	vector<size_t> muls {
		10,
		25,
		50,
	};

	vector<size_t> result;
	size_t d = 10;
	while (d) {
		for (auto m : muls) {
			auto opt = d*m;

			if (!g_deviceIsScope && opt <= 1000) {
				// DSLogic won't actually take captures under 2.5kS
				// There doesn't seem to be any way to get the sample depth
				// options programmatically...
				continue;
			}

			if (opt > g_hw_depth || opt > 5000000) {
				// Reported hardware depth limit is astonishing.
				// I can't configure it above 5MS on my laptop without getting
				// a LIBUSB_ERROR_NO_MEM inside libsigrok4DSL, so arbitrary
				// limit there for now.
				d = 0;
				break;
			}

			result.push_back(opt);
		}

		d *= 10;
	}

	return result;
}

bool SigrokSCPIServer::OnCommand(
		const string& line,
		const string& subject,
		const string& cmd,
		const std::vector<std::string>& args)
{
	if(BridgeSCPIServer::OnCommand(line, subject, cmd, args))
		return true;

	//TODO: handle commands not implemented by the base class
	LogWarning("Unrecognized command received: %s\n", line.c_str());

	return false;
}

//-- Acquisition Commands --//
/**
	@brief Arm the device for capture. If oneShot, capture only one waveform
 */
void SigrokSCPIServer::AcquisitionStart(bool oneShot)
{
	LogDebug("cmd: START\n");

	g_oneShot = oneShot;

	g_seqnum = 0;
	g_session_start_ms = get_ms();
	g_lastReportedRate = 0;

	g_run = true;

	force_correct_config();


}

/**
	@brief Force the device to capture a waveform
 */
void SigrokSCPIServer::AcquisitionForceTrigger()
{
	AcquisitionStart(true);
}

/**
	@brief Stop the device from capturing further waveforms
 */
void SigrokSCPIServer::AcquisitionStop()
{
	LogDebug("cmd: STOP\n");

	g_run = false;
	sr_session_stop();
}


//-- Probe Configuration --//
/**
	@brief Enable or disable channel `chIndex`, enable if `enabled==true`
 */
void SigrokSCPIServer::SetChannelEnabled(size_t chIndex, bool enabled)
{
	if (!g_deviceIsScope) return;

	if (!enabled && chIndex == 0 && !get_probe_config<bool>(g_sr_device, g_channels[1], SR_CONF_PROBE_EN)) {
		LogWarning("Ignoring request to disable ch0 because it would disable all channels\n");
	} else {
		// Must stop acquisition while disabling probe or we crash inside vendor code
		bool wasRunning = stop_capture_sync();

		set_probe_config<bool>(g_sr_device, g_channels[chIndex], SR_CONF_PROBE_EN, enabled);
		LogDebug("Updated ENABLED for ch%ld, now %d\n", chIndex, enabled);

		force_correct_config();
		
		if (wasRunning) restart_capture();

		force_correct_config();
	}
}

/**
	@brief Set the coupling of the probe on channel `chIndex` to `coupling`
 */
void SigrokSCPIServer::SetAnalogCoupling(size_t chIndex, const std::string& coupling)
{
	if (!g_deviceIsScope) return;

	uint8_t sr_coupling = -1;

	if (coupling == "AC1M") {
		sr_coupling = SR_AC_COUPLING;
	} else if (coupling == "DC1M") {
		sr_coupling = SR_DC_COUPLING;
	} else {
		LogWarning("Unknown coupling: %s\n", coupling.c_str());
		return;
	}

	set_probe_config<uint8_t>(g_sr_device, g_channels[chIndex], SR_CONF_PROBE_COUPLING, sr_coupling);
	LogDebug("Updated coupling for ch%ld, now %s\n", chIndex, coupling.c_str());
}

/**
	@brief Set the requested voltage range of the probe on channel `chIndex`
	       to `range` (Volts max-to-min)
 */
void SigrokSCPIServer::SetAnalogRange(size_t chIndex, double range_V)
{
	if (!g_deviceIsScope) return;

	float range_mV_per_div = (range_V / g_numdivs) * 1000;

	uint64_t selected = vdiv_options[vdiv_options.size()];

	for (auto i : vdiv_options) {
		if ((i > selected) && !(i > range_mV_per_div)) {
			selected = i;
		}
	}

	set_probe_config<uint64_t>(g_sr_device, g_channels[chIndex], SR_CONF_PROBE_VDIV, selected);
	LogDebug("Updated RANGE; Wanted %f (%f PtP), result: %lu\n", range_mV_per_div, range_V, selected);
}

/**
	@brief Set the threshold for a digital HIGH on the channel `chIndex`
 */
void SigrokSCPIServer::SetDigitalThreshold(size_t chIndex, double threshold_V)
{
	if (g_deviceIsScope) return;
	
	if (threshold_V < 0) threshold_V = 0;
	if (threshold_V > 5) threshold_V = 5;

	set_dev_config<double>(g_sr_device, SR_CONF_VTH, threshold_V);

	LogDebug("Updated THRESH, now %f\n", threshold_V);
}

/**
	@brief Set the hysteresis value for the channel `chIndex`
 */
void SigrokSCPIServer::SetDigitalHysteresis(size_t chIndex, double hysteresis)
{
	;
}

/**
	@brief Set the requested voltage offset of the probe on channel `chIndex`
	       to `offset` (Volts)
 */
void SigrokSCPIServer::SetAnalogOffset(size_t chIndex, double offset_V)
{
	;
}

//-- Sampling Configuration --//
/**
	@brief Set sample rate in Hz
 */
void SigrokSCPIServer::SetSampleRate(uint64_t rate_hz)
{
	set_rate(rate_hz);

	LogDebug("Updated RATE; now %lu\n", g_rate);

	force_correct_config();
}

/**
	@brief Set sample rate in samples
 */
void SigrokSCPIServer::SetSampleDepth(uint64_t depth)
{
	set_depth(depth);

	LogDebug("Updated DEPTH; now %lu\n", g_depth);

	force_correct_config();
}

//-- Trigger Configuration --//
/**
	@brief Set trigger delay in femptoseconds
 */
void SigrokSCPIServer::SetTriggerDelay(uint64_t delay_fs)
{
	set_trigfs(delay_fs);

	LogDebug("Set trigger DELAY to %lu (%%%d)\n", delay_fs, g_trigpct);
}

/**
	@brief Set trigger source to the probe on channel `chIndex`
 */
void SigrokSCPIServer::SetTriggerSource(size_t chIndex)
{
	set_trigger_channel(chIndex);

	LogDebug("Set trigger SOU to %lu\n", chIndex);
}

//-- (Edge) Trigger Configuration --//
/**
	@brief Configure the device to use an edge trigger
 */
void SigrokSCPIServer::SetTriggerTypeEdge()
{
	;
}

/**
	@brief Set the edge trigger's level to `level` in Volts
 */
void SigrokSCPIServer::SetTriggerLevel(double level_V)
{
	if (!g_deviceIsScope) return;

	// Set it on all probes, allowing SR_CONF_TRIGGER_SOURCE to select
	// which is actually active
	for (auto ch : g_channels) {
		float scale, offset;
		compute_scale_and_offset(ch, scale, offset);

		// voltage = ADC * scale - offset
		// ADC = (voltage + offset) / scale

		uint8_t adc = (level_V + offset) / scale;

		set_probe_config<uint8_t>(g_sr_device, ch, SR_CONF_TRIGGER_VALUE, adc);
	}

	LogDebug("Set trigger LEV to %f\n", level_V);
}

/**
	@brief Set the edge trigger's activation to the edge `edge`
	       ("RISING", "FALLING", ...)
 */
void SigrokSCPIServer::SetEdgeTriggerEdge(const std::string& edge)
{
	int dir = -1;

	if (edge == "RISING") {
		dir = RISING;
	} else if (edge == "FALLING") {
		dir = FALLING;
	} else if (edge == "ANY" && !g_deviceIsScope) {
		dir = ANY;
	} else {
		LogWarning("Unsupported trigger EDGE: %s\n", edge.c_str());
		return;
	}

	set_trigger_direction(dir);

	LogDebug("Set trigger EDGE to %s\n", edge.c_str());
}

