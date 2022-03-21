
#include "srbinding.h"

#include <stdint.h>
#include <glib.h>
#include <string>
#include <map>

#define BINDING_TYPES_X(X) \
 X(uint64_t, UINT64, uint64) \
 X(uint32_t, UINT32, uint32) \
 X(uint16_t, UINT16, uint16) \
 X(uint8_t, BYTE, byte) \
 X(bool, BOOLEAN, boolean) \
 X(double, DOUBLE, double)

template <typename T>
bool extract_gvar(GVariant* gvar, T& result);

template <typename T>
GVariant* make_gvar(T value);

#define X_MAKE_SPEC(CTYPE, GTYPELIT, GTYPEFUNC) \
template <> \
bool extract_gvar<CTYPE>(GVariant* gvar, CTYPE& result) { \
    if (gvar != NULL && g_variant_is_of_type(gvar, G_VARIANT_TYPE_##GTYPELIT)) { \
        result = g_variant_get_##GTYPEFUNC(gvar); \
        g_variant_unref(gvar); \
        return true; \
    } \
    return false; \
} \
template <> \
GVariant* make_gvar<CTYPE>(CTYPE value) { \
    return g_variant_new_##GTYPEFUNC(value); \
}

BINDING_TYPES_X(X_MAKE_SPEC)

template <typename T>
std::optional<T> get_probe_config(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key) {
	GVariant* gvar = NULL;
	sr_config_get(dev->driver, (struct sr_dev_inst*) dev, (struct sr_channel*) ch, NULL, key, &gvar);
    T result;
	if (extract_gvar<T>(gvar, result)) {
        return std::optional{result};
    }

    return std::nullopt;
}

template <typename T>
bool set_probe_config(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key, T value) {
	GVariant* gvar = make_gvar<T>(value);
	int err = sr_config_set((struct sr_dev_inst*) dev, (struct sr_channel*) ch, NULL, key, gvar);

	return err != 0;
}

std::map<int, std::string> dev_config_indirections {
    {SR_CONF_PROBE_VDIV, "vdivs"},
    {SR_CONF_SAMPLERATE, "samplerates"}
};

template <typename T>
std::vector<T> get_dev_config_options(const struct sr_dev_inst* dev, int key) {
    GVariant* list;
    GVariant* orig_list = NULL;
    std::vector<T> values;
    if (sr_config_list(dev->driver, dev, NULL, key, &list) != SR_OK) {
        printf("Failed to interrogate device in get_dev_config_options\n");
        return values;
    }

    if (dev_config_indirections.count(key)) {
        orig_list = list;
        list = g_variant_lookup_value(orig_list, dev_config_indirections[key].c_str(), NULL);
    }

    GVariantIter iterator;
    GVariant* thisItem;
    g_variant_iter_init(&iterator, list);

    for (;;) {
        thisItem = g_variant_iter_next_value(&iterator);
        if (thisItem == NULL) break;

        T result;
        if (extract_gvar<T>(thisItem, result)) {
            values.push_back(result);
        } else {
            printf("Item of unexpected type in get_dev_config_options\n");
        }
    }

    g_variant_unref(list);

    if (orig_list) {
        g_variant_unref(orig_list);
    }

    return values;
}

#define X_FORCE_INSTANTIATION(CTYPE, GTYPELIT, GTYPEFUNC) \
template std::optional<CTYPE> get_probe_config<CTYPE>(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key); \
template bool set_probe_config<CTYPE>(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key, CTYPE value); \
template std::vector<CTYPE> get_dev_config_options<CTYPE>(const struct sr_dev_inst* dev, int key);

BINDING_TYPES_X(X_FORCE_INSTANTIATION);
