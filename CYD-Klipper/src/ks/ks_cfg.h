// Persistent KlipperScreen options (the [main] section of KlipperScreen.conf), stored in NVS namespace "ks".
#pragma once
bool ks_cfg_bool(const char* key, bool def);
void ks_cfg_set_bool(const char* key, bool v);
int  ks_cfg_int(const char* key, int def);
void ks_cfg_set_int(const char* key, int v);
