
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <vector>

#include <libsigrok4DSL/libsigrok.h>

#include "log/log.h"

#include "srbinding.h"
#include "server.h"

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

	ds_trigger_init();

	int bitdepth = get_dev_config<uint8_t>(device, SR_CONF_UNIT_BITS).value();
	LogDebug("Bit depth: %d\n", bitdepth);

	// int refmin = get_dev_config<uint32_t>(device, SR_CONF_REF_MIN).value_or(-1);
	// int refmax = get_dev_config<uint32_t>(device, SR_CONF_REF_MAX).value_or(-1);
	// LogDebug("Ref min/max: %d/%d\n", refmin, refmax);

	run_server(device, 4000);

	(void) session;

	return 0;
}
