#pragma once
// "Is this pointer in internal RAM?" -- on a PC every pointer is, in the only
// sense the question is asked: it is not behind a flash cache.
static inline bool esp_ptr_internal(const void*) { return true; }
