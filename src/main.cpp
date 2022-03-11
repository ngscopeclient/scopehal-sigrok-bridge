
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <vector>

#include <libsigrok4DSL/libsigrok.h>

#include "log/log.h"

#include "srbinding.h"
#include "server.h"

void callback (const struct sr_dev_inst *sdi, const struct sr_datafeed_packet *packet, void *cb_data) {
	(void) sdi;
	(void) cb_data;

	if (packet->type == SR_DF_DSO) {
		struct sr_datafeed_dso* dso = (struct sr_datafeed_dso*)packet->payload;
		int num_samples = dso->num_samples;
		LogDebug("Received %d samples\n", num_samples);

		uint8_t* buf = (uint8_t*) dso->data;

		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 16; i++) {
				int c = *(buf+=1) - (1<<7);
				LogDebug("%+02d ", c);
			}
			LogDebug("\n");
		}
	}
}

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

int main(int argc, char* argv[])
{
	(void) argc; (void) argv;

	Severity console_verbosity = Severity::DEBUG;
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	chdir("/usr/local/share/DSView/res/");

	LogNotice("libsigrok4DSL ver: '%s'\n", sr_package_version_string_get());
	// sr_log_loglevel_set(4);
	   //    0   None
	   //    1   Error
	   //    2   Warnings
	   //    3   Informational
	   //    4   Debug
	   //    5   Spew

	struct sr_context* context;
	int err;
	if ((err = sr_init(&context)) != SR_OK) {
		LogError("Failed to initialize libsigrok4DSL: %d\n", err);
		return err;
	}

	struct sr_dev_driver* sel_driver;

	const char* wanted_driver = "DSCope";
	// virtual-demo, DSLogic, DSCope

	sr_dev_driver **const drivers = sr_driver_list();
	for (sr_dev_driver **driver = drivers; *driver; driver++) {
		if (strcmp((*driver)->name, wanted_driver) == 0) {
			sel_driver = *driver;
			break;
		}
	}

	if (sr_driver_init(context, sel_driver) != SR_OK) {
		LogError("Failed to initialize driver %s\n", sel_driver->name);
		return 1;
	}

	LogNotice("Selected driver %s, scanning...\n", sel_driver->name);

	GSList *const devices = sr_driver_scan(sel_driver, NULL);

	struct sr_dev_inst *device = NULL;

	for (GSList *l = devices; l; l = l->next) {
        device = (sr_dev_inst*)l->data;
        LogNotice("Found device: %s - %s\n", device->vendor, device->model);
    }

    if (!device) {
    	LogError("Found no device\n");
    	return 1;
    }
     
	g_slist_free(devices);

	if ((err = sr_dev_open(device)) != SR_OK) {
		LogError("Failed to sr_dev_open device: %d\n", err);
		return 1;
	}

	struct sr_session* session = sr_session_new();

	if ((err = sr_session_dev_add(device)) != SR_OK) {
		LogError("Failed to add device to session\n");
		return 1;
	}

	sr_session_datafeed_callback_add(callback, NULL);

	ds_trigger_init();

	std::vector<struct sr_channel*> channels;
	for (GSList *l = device->channels; l; l = l->next) {
        struct sr_channel* ch = (struct sr_channel*)l->data;
        channels.push_back(ch);
    }

    auto ch0 = channels.at(0);

    for(auto ch : channels) {
    	set_probe_config<bool>(device, ch, SR_CONF_PROBE_EN, ch == ch0);
    }

    set_probe_config<uint64_t>(device, ch0, SR_CONF_PROBE_VDIV, 100);
    set_probe_config<uint8_t>(device, ch0, SR_CONF_PROBE_COUPLING, SR_AC_COUPLING);

    for (auto ch : channels) {
    	bool en = get_probe_config<bool>(device, ch, SR_CONF_PROBE_EN).value();
    	LogDebug("Ch %s: %s\n", ch->name, en?"ENABLED":"DISABLED");
    	if (en) {
    		std::vector<uint64_t> vdivs;
    		populate_vdivs(device, vdivs);
    		for (uint64_t i : vdivs) {
    			LogDebug(" - vdiv option: %lu\n", i);
    		}

    		uint64_t active = get_probe_config<uint64_t>(device, ch, SR_CONF_PROBE_VDIV).value();
    		LogDebug("Active: %lu\n", active);
    	}
    }

	int bitdepth = get_dev_config<uint8_t>(device, SR_CONF_UNIT_BITS).value();
	LogDebug("Bit depth: %d\n", bitdepth);

	set_dev_config<uint64_t>(device, SR_CONF_SAMPLERATE, 5000);

	uint64_t samplerate = get_probe_config<uint64_t>(device, NULL, SR_CONF_SAMPLERATE).value();
	LogDebug("Sample Rate: %lu\n", samplerate);

	int refmin = get_dev_config<uint32_t>(device, SR_CONF_REF_MIN).value_or(-1);
	int refmax = get_dev_config<uint32_t>(device, SR_CONF_REF_MAX).value_or(-1);
	LogDebug("Ref min/max: %d/%d\n", refmin, refmax);

	run_server(device, 4000);

	// if ((err = sr_session_start()) != SR_OK) {
	// 	LogError("session_start returned failure: %d\n", err);
	// 	return 1;
	// }
	// if ((err = sr_session_run()) != SR_OK) {
	// 	LogError("session_run returned failure: %d\n", err);
	// 	return 1;
	// }

	(void) session;

	return 0;
}
