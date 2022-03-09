
#include "srbinding.h"

#include <stdint.h>
#include <glib.h>

#define BINDING_TYPES_X(X) \
 X(uint64_t, UINT64, uint64) \
 X(uint32_t, UINT32, uint32) \
 X(uint16_t, UINT16, uint16) \
 X(uint8_t, BYTE, byte) \
 X(bool, BOOLEAN, boolean)

#define X_MAKE_SPEC(CTYPE, GTYPELIT, GTYPEFUNC) \
template <>\
std::optional<CTYPE> get_probe_config<CTYPE>(struct sr_dev_inst* dev, struct sr_channel* ch, int key) {\
	GVariant* gvar = NULL;\
	sr_config_get(dev->driver, dev, ch, NULL, key, &gvar);\
	if (gvar != NULL && g_variant_is_of_type(gvar, G_VARIANT_TYPE_##GTYPELIT)) {\
        CTYPE result = g_variant_get_##GTYPEFUNC(gvar);\
        g_variant_unref(gvar);\
\
        return std::optional{result};\
    }\
\
    return std::nullopt;\
}\
\
template <>\
bool set_probe_config<CTYPE>(struct sr_dev_inst* dev, struct sr_channel* ch, int key, CTYPE value) {\
	GVariant* gvar = g_variant_new_##GTYPEFUNC(value);\
	int err = sr_config_set(dev, ch, NULL, key, gvar);\
\
	return err != 0;\
}

BINDING_TYPES_X(X_MAKE_SPEC)
