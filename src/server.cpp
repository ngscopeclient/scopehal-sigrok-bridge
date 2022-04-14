
#include "server.h"
#include <libsigrok4DSL/libsigrok.h>
#include "log/log.h"
#include "srbinding.h"
#include <math.h>

struct sr_context* g_sr_context = NULL;
struct sr_dev_inst* g_sr_device = NULL;
struct sr_session* g_sr_session = NULL;

std::vector<struct sr_channel*> g_channels{};
std::vector<uint64_t> vdiv_options{};
std::vector<uint64_t> g_attenuations{};

bool g_quit;
bool g_run;
bool g_running;
bool g_oneShot;
bool g_capturedFirstFrame;

bool g_deviceIsScope;

uint64_t g_rate, g_depth, g_trigfs = 0;
uint64_t g_hw_depth;
uint8_t g_trigpct = 0;

int g_selectedTriggerChannel;
int g_selectedTriggerDirection;
int g_numdivs = DS_CONF_DSO_VDIVS;
uint32_t g_hwmin, g_hwmax;
float g_hwrange_factor;
// TODO: SR_CONF_NUM_VDIV instead of DS_CONF_DSO_VDIVS on regular sigrok

void update_trigger_internals();

void set_trigger_channel(int ch) {
	set_dev_config<uint8_t>(g_sr_device, SR_CONF_TRIGGER_SOURCE, ch==0?DSO_TRIGGER_CH0:DSO_TRIGGER_CH1);
	g_selectedTriggerChannel = ch;

	if (!g_deviceIsScope)
		update_trigger_internals();
}

void set_trigger_direction(int dir) {
	if (g_deviceIsScope) {
		int sr_edge = -1;

		if (dir == RISING) {
			sr_edge = DSO_TRIGGER_RISING;
		} else if (dir == FALLING) {
			sr_edge = DSO_TRIGGER_FALLING;
		} else {
			return;
		}

		set_dev_config<uint8_t>(g_sr_device, SR_CONF_TRIGGER_SLOPE, sr_edge);
	} else {
		g_selectedTriggerDirection = dir;
		update_trigger_internals();
	}
}


void update_trigger_internals() {
	for (int ch = 0; ch < (int)g_channels.size(); ch++) {
		char dir = 'X';
		if (g_selectedTriggerChannel == ch) {
			if (g_selectedTriggerDirection == RISING)
				dir = 'R';
			else if (g_selectedTriggerDirection == FALLING)
				dir = 'F';
			else if (g_selectedTriggerDirection == ANY)
				dir = 'C';
		}

		ds_trigger_probe_set(ch, dir, 'X');
	}

	force_correct_config();
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

	float full_throw_V = vdiv_mV / 1000 * g_numdivs;  // Volts indicated by most-positive value (255)
	// g_hwrange_factor adjusts for incomplete range of ADC reports
	scale = -1 * g_hwrange_factor / 255.f * full_throw_V; // ADC values are 'upside down'
	offset = 128 * scale; // Zero is 128

	// TODO: In fact, as vertical scale zooms out (vdiv_mV increases) the ADC samples seem to become
	//       more and more biased towards negative... this is observable in DSView too, so may just
	//       be a result of the hardware design. Not attempting to compensate for now.
}

// Not exposed from the DSL driver code, so copied here...
enum LANGUAGE {
    LANGUAGE_CN = 25,
    LANGUAGE_EN = 31,
};

int init_and_find_device(const char* wanted_driver, int req_usb_bus, int req_usb_dev) {
	int err;
	if ((err = sr_init(&g_sr_context)) != SR_OK) {
		LogError("Failed to initialize libsigrok4DSL: %d\n", err);
		return err;
	}

	g_sr_device = NULL;

	struct sr_dev_driver* sel_driver = NULL;

	sr_dev_driver **const drivers = sr_driver_list();
	for (sr_dev_driver **driver = drivers; *driver; driver++) {
		if (strcmp((*driver)->name, wanted_driver) == 0) {
			sel_driver = *driver;
			break;
		}
	}

	if (!sel_driver) {
		LogError("Didn't find driver.\n");
		return 1;
	}

	if (sr_driver_init(g_sr_context, sel_driver) != SR_OK) {
		LogError("Failed to initialize driver %s\n", sel_driver->name);
		return 1;
	}

	LogNotice("Selected driver %s, scanning...\n", sel_driver->name);

	GSList *const devices = sr_driver_scan(sel_driver, NULL);

	int dev_usb_bus, dev_usb_dev;

	for (GSList *l = devices; l; l = l->next) {
		sr_dev_inst* dev = (sr_dev_inst*)l->data;
        // SORRY! SORRY! SORRY! This struct is only available in libsigrok-internal.h
		// TODO: NOT portable to regular sigrok
		uint8_t* p = (uint8_t*)dev->conn;
		dev_usb_bus = *p++;
		dev_usb_dev = *p++;

		bool ok = true;

		if (req_usb_bus != -1) {
			ok = req_usb_bus == dev_usb_bus && req_usb_dev == dev_usb_dev;
		}

		if (ok) {
        	g_sr_device = dev;
        	break;
        }
    }

    if (!g_sr_device) {
    	LogError("Found no device\n");
    	return 1;
    }
     
	g_slist_free(devices);

	LogNotice("Found device: %s - %s\n", g_sr_device->vendor, g_sr_device->model);
	LogDebug(" -> USB bus %d : dev %d\n", dev_usb_bus, dev_usb_dev);

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

	uint8_t numbits = get_dev_config<uint8_t>(g_sr_device, SR_CONF_UNIT_BITS).value();
	LogDebug("Sample bits: %d", numbits);

	if (numbits == 1) {
		LogDebug(" (logic analyzer)\n");
		g_deviceIsScope = false;
	} else if (numbits == 8) {
		LogDebug(" (scope)\n");
		g_deviceIsScope = true;
	} else {
		LogError("\n -> Unsupported bit depth/device type\n");
		return 1;
	}

	for (GSList *l = g_sr_device->channels; l; l = l->next) {
        struct sr_channel* ch = (struct sr_channel*)l->data;
        g_channels.push_back(ch);
        g_attenuations.push_back(1);
    }

    LogDebug("Device has %ld channels\n", g_channels.size());

    // Must configure device language to make SR_CONF_OPERATION_MODE values meaningful...
	set_dev_config<int16_t>(g_sr_device, SR_CONF_LANGUAGE, LANGUAGE_EN);

	for (std::string opt : get_dev_config_options<std::string>(g_sr_device, SR_CONF_OPERATION_MODE))
	{
		LogDebug(" - available operation mode: %s\n", opt.c_str());
	}

	LogDebug("Initial op mode: %s; stream = %d\n",
		get_dev_config<std::string>(g_sr_device, SR_CONF_OPERATION_MODE).value().c_str(),
		get_dev_config<bool>(g_sr_device, SR_CONF_STREAM).value());

	if (g_deviceIsScope) {
		vdiv_options = get_dev_config_options<uint64_t>(g_sr_device, SR_CONF_PROBE_VDIV);
		LogDebug("vdiv options: ");
		for (auto opt : vdiv_options) {
			LogDebug("%ld (%.1fV), ", opt, ((float)opt)/1000*g_numdivs);
		}
		LogDebug("\n");
	} else {
		set_dev_config<std::string>(g_sr_device, SR_CONF_OPERATION_MODE, "Buffer Mode");

		LogDebug("Configured op mode: %s; stream = %d\n",
			get_dev_config<std::string>(g_sr_device, SR_CONF_OPERATION_MODE).value().c_str(),
			get_dev_config<bool>(g_sr_device, SR_CONF_STREAM).value());

		ds_trigger_set_mode(SIMPLE_TRIGGER);
		ds_trigger_set_en(true);
	}

	g_hw_depth = get_dev_config<uint64_t>(g_sr_device, SR_CONF_HW_DEPTH).value();
	// LogDebug("Hardware depth limit: %lu (1<<%d)\n", g_hw_depth, (int)log2(g_hw_depth));
	// This seems like a meaningless number (way too high)

	g_hwmin = get_dev_config<uint32_t>(g_sr_device, SR_CONF_REF_MIN).value_or(0);
	g_hwmax = get_dev_config<uint32_t>(g_sr_device, SR_CONF_REF_MAX).value_or((1 << 8) - 1);
	g_hwrange_factor = (255.f / (g_hwmax - g_hwmin));
	// TODO: Actual ADC samples on my DSCope extend to 0x03 and 0xFC (this reports 0x0A - 0xF5)...
	//       ignoring for now / treating that as clipping.
	LogDebug("Hardware ADC report range: %02x - %02x (adj: %.3f)\n", g_hwmin, g_hwmax, g_hwrange_factor);
	if (g_hwmin != 0 && g_hwmax != 255) LogDebug(" (Clipping detection supported)\n");


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

	int cycles = 0;
	while (g_running) {
		if (cycles++ > 1000) break; // Avoid hanging if not triggering
		usleep(100);
	}

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

		int cycles = 0;
		while (!g_capturedFirstFrame) {
			if (cycles++ > 1000) break; // Avoid hanging if not triggered
			usleep(100);
		}
	}

	set_rate(g_rate);
	set_depth(g_depth);
	set_trigfs(g_trigfs);

	
    set_probe_config<uint64_t>(g_sr_device, g_channels[0], SR_CONF_PROBE_FACTOR, 10);
    set_probe_config<uint64_t>(g_sr_device, g_channels[1], SR_CONF_PROBE_FACTOR, 1);

	bool wasRunning = stop_capture_sync();
	if (wasRunning) restart_capture();
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

	double pct;
	if (fs != 0) {
		int numchans = count_enabled_channels();

		uint64_t samples_in_full_capture = get_dev_config<uint64_t>(g_sr_device, SR_CONF_LIMIT_SAMPLES).value();
		uint64_t samplerate_hz = get_dev_config<uint64_t>(g_sr_device, SR_CONF_SAMPLERATE).value();

		if (samplerate_hz == 1000000000 && numchans == 2) {
			// Seems to incorrectly report a 1Gs/s rate on both channels when it is actually 1Gs/s TOTAL
			samplerate_hz /= 2;
		}

		uint64_t fs_per_sample = 1000000000000000 / samplerate_hz;
		uint64_t fs_in_full_capture = samples_in_full_capture * fs_per_sample;
		pct = ((double)fs / (double)fs_in_full_capture) * (double)100;
	} else {
		pct = 0;
	}

	g_trigpct = pct;

	// LogWarning("samples=%lu, hz=%lu, fsper=%lu, fsinfull=%lu, pct=%f\n", 
	// 	samples_in_full_capture, samplerate_hz, fs_per_sample, fs_in_full_capture, pct);

	if (pct > 100 || pct < 0) {
		set_trigfs(0);
		return;
	}

	if (g_deviceIsScope)
		set_dev_config<uint8_t>(g_sr_device, SR_CONF_HORIZ_TRIGGERPOS, g_trigpct);
	else
		ds_trigger_set_pos(g_trigpct);
}
