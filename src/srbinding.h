
#ifndef SRBINDING_H
#define SRBINDING_H

#include <libsigrok4DSL/libsigrok.h>
#include <optional>

template <typename T>
std::optional<T> get_probe_config(struct sr_dev_inst* dev, struct sr_channel* ch, int key);

template <typename T>
bool set_probe_config(struct sr_dev_inst* dev, struct sr_channel* ch, int key, T value);

template <typename T>
static inline std::optional<T> get_dev_config(struct sr_dev_inst* dev, int key) {
	return get_probe_config<T>(dev, NULL, key);
}

template <typename T>
static inline bool set_dev_config(struct sr_dev_inst* dev, int key, T value) {
	return set_probe_config<T>(dev, NULL, key, value);
}

#endif
