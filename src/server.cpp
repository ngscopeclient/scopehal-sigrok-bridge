
#include "server.h"
#include <libsigrok4DSL/libsigrok.h>
#include "log/log.h"
#include "srbinding.h"

struct sr_context* g_sr_context = NULL;
struct sr_dev_inst* g_sr_device = NULL;
struct sr_session* g_sr_session = NULL;

std::vector<struct sr_channel*> g_channels{};
std::vector<uint64_t> vdiv_options{};

bool g_quit;
bool g_run;
bool g_running;
bool g_oneShot;
bool g_capturedFirstFrame;

bool g_deviceIsScope;

uint64_t g_rate, g_depth, g_trigfs = 0;
uint8_t g_trigpct = 0;

int g_selectedTriggerChannel;
int g_numdivs = DS_CONF_DSO_VDIVS;
// TODO: SR_CONF_NUM_VDIV instead of DS_CONF_DSO_VDIVS on regular sigrok

void set_trigger_channel(int ch) {
	set_dev_config<uint8_t>(g_sr_device, SR_CONF_TRIGGER_SOURCE, ch==0?DSO_TRIGGER_CH0:DSO_TRIGGER_CH1);
	g_selectedTriggerChannel = ch;
}

int count_enabled_channels() {
	int enabled = 0;

	for (auto ch : g_channels) {
		if (get_probe_config<bool>(g_sr_device, ch, SR_CONF_PROBE_EN) && ch->enabled) {
			// SR_CONF_PROBE_EN seems to not report correct result
			enabled++;
		}
	}

	return enabled;
}

void compute_scale_and_offset(struct sr_channel* ch, float& scale, float& offset) {
	float vdiv_mV = get_probe_config<uint64_t>(g_sr_device, ch, SR_CONF_PROBE_VDIV).value();
	float hwmin = get_dev_config<uint32_t>(g_sr_device, SR_CONF_REF_MIN).value_or(0);
	float hwmax = get_dev_config<uint32_t>(g_sr_device, SR_CONF_REF_MAX).value_or((1 << 8) - 1);
	float full_throw_V = vdiv_mV / 1000 * g_numdivs;  // Volts indicated by most-positive value (255)
	float hwrange_factor = (255.f / (hwmax - hwmin)); // Adjust for incomplete range of ADC reports
	scale = -1 * hwrange_factor / 255.f * full_throw_V; // ADC values are 'upside down'
	offset = 127 * scale; // Zero is 127
}

int init_and_find_device() {
	int err;
	if ((err = sr_init(&g_sr_context)) != SR_OK) {
		LogError("Failed to initialize libsigrok4DSL: %d\n", err);
		return err;
	}

	struct sr_dev_driver* sel_driver;

	const char* wanted_driver = "DSLogic";
	// virtual-demo, DSLogic, DSCope

	sr_dev_driver **const drivers = sr_driver_list();
	for (sr_dev_driver **driver = drivers; *driver; driver++) {
		if (strcmp((*driver)->name, wanted_driver) == 0) {
			sel_driver = *driver;
			break;
		}
	}

	if (sr_driver_init(g_sr_context, sel_driver) != SR_OK) {
		LogError("Failed to initialize driver %s\n", sel_driver->name);
		return 1;
	}

	LogNotice("Selected driver %s, scanning...\n", sel_driver->name);

	GSList *const devices = sr_driver_scan(sel_driver, NULL);

	for (GSList *l = devices; l; l = l->next) {
        g_sr_device = (sr_dev_inst*)l->data;
    }

    if (!g_sr_device) {
    	LogError("Found no device\n");
    	return 1;
    }
     
	g_slist_free(devices);

	LogNotice("Found device: %s - %s\n", g_sr_device->vendor, g_sr_device->model);

	if ((err = sr_dev_open(g_sr_device)) != SR_OK) {
		LogError("Failed to sr_dev_open device: %d\n", err);
		return 1;
	}

	g_sr_session = sr_session_new();

	if ((err = sr_session_dev_add(g_sr_device)) != SR_OK) {
		LogError("Failed to add device to session\n");
		return 1;
	}

	ds_trigger_init();

	// Relevant on DSLogic. Let's work in non-stream mode for now.
	// This is also required for non 0% delay
	set_dev_config<bool>(g_sr_device, SR_CONF_STREAM, false);

	uint8_t numbits = get_dev_config<uint8_t>(g_sr_device, SR_CONF_UNIT_BITS).value();
	LogDebug("Sample bits: %d\n", numbits);

	if (numbits == 1) {
		g_deviceIsScope = false;
	} else if (numbits == 8) {
		g_deviceIsScope = true;
	} else {
		LogError("Unsupported bit depth/device type\n");
	}

	for (GSList *l = g_sr_device->channels; l; l = l->next) {
        struct sr_channel* ch = (struct sr_channel*)l->data;
        g_channels.push_back(ch);
    }

    LogDebug("Device has %ld channels\n", g_channels.size());

    if (g_deviceIsScope)
		vdiv_options = get_dev_config_options<uint64_t>(g_sr_device, SR_CONF_PROBE_VDIV);

    set_trigger_channel(0);
    set_rate(10000000);
    set_depth(1000);

    return 0;
}

bool stop_capture_sync() {
	if (!g_run && !g_running) return false;

	bool wasRunning = g_run;

	g_run = false;
	sr_session_stop();

	while (g_running) usleep(100);

	return wasRunning;
}

void restart_capture() {
	g_run = true;
}

void force_correct_config() {
	// Why this dance is required, I don't know. The first time the system starts
	// and occasionally otherwise it seems to reset itself to 1MS/s 1Mpts configuration
	// and this fixes it.

	if (g_deviceIsScope && g_run) {
		while (!g_running) usleep(100);

		while (!g_capturedFirstFrame) usleep(100);
	}

	set_rate(g_rate);
	set_depth(g_depth);
	set_trigfs(g_trigfs);

	if (g_deviceIsScope) {
		bool wasRunning = stop_capture_sync();
		if (wasRunning) restart_capture();
	}
}

void set_rate(uint64_t rate) {
	// LogDebug("set_rate: %lu\n", rate);
	g_rate = rate;
	set_dev_config<uint64_t>(g_sr_device, SR_CONF_SAMPLERATE, g_rate);
}

void set_depth(uint64_t depth) {
	// LogDebug("set_depth: %lu\n", depth);
	g_depth = depth;
	set_dev_config<uint64_t>(g_sr_device, SR_CONF_LIMIT_SAMPLES, g_depth);
}

void set_trigfs(uint64_t fs) {
	// LogDebug("set_trigfs %lu\n", fs);
	g_trigfs = fs;

	int numchans = count_enabled_channels();

	uint64_t samples_in_full_capture = get_dev_config<uint64_t>(g_sr_device, SR_CONF_LIMIT_SAMPLES).value();
	uint64_t samplerate_hz = get_dev_config<uint64_t>(g_sr_device, SR_CONF_SAMPLERATE).value();

	if (samplerate_hz == 1000000000 && numchans == 2) {
		// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
		samplerate_hz /= 2;
	}

	uint64_t fs_per_sample = 1000000000000000 / samplerate_hz;
	uint64_t fs_in_full_capture = samples_in_full_capture * fs_per_sample;
	double pct = ((double)fs / (double)fs_in_full_capture) * (double)100;
	g_trigpct = pct;

	// LogWarning("samples=%lu, hz=%lu, fsper=%lu, fsinfull=%lu, pct=%f\n", 
	// 	samples_in_full_capture, samplerate_hz, fs_per_sample, fs_in_full_capture, pct);

	set_dev_config<uint8_t>(g_sr_device, SR_CONF_HORIZ_TRIGGERPOS, g_trigpct);
}
