
#ifndef SRBINDING_H
#define SRBINDING_H

#include <libsigrok4DSL/libsigrok.h>
#include <optional>
#include <vector>

template <typename T>
std::optional<T> get_probe_config(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key);

template <typename T>
bool set_probe_config(const struct sr_dev_inst* dev, const struct sr_channel* ch, int key, T value);

template <typename T>
inline std::optional<T> get_dev_config(const struct sr_dev_inst* dev, int key) {
	return get_probe_config<T>(dev, NULL, key);
}

template <typename T>
inline bool set_dev_config(const struct sr_dev_inst* dev, int key, T value) {
	return set_probe_config<T>(dev, NULL, key, value);
}

template <typename T>
std::vector<T> get_dev_config_options(const struct sr_dev_inst* dev, int key);

#endif
