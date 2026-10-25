#include "ks_cfg.h"
#include <Preferences.h>

bool ks_cfg_bool(const char* key, bool def) { Preferences p; p.begin("ks", true); bool v = p.getBool(key, def); p.end(); return v; }
void ks_cfg_set_bool(const char* key, bool v) { Preferences p; p.begin("ks", false); p.putBool(key, v); p.end(); }
int  ks_cfg_int(const char* key, int def) { Preferences p; p.begin("ks", true); int v = p.getInt(key, def); p.end(); return v; }
void ks_cfg_set_int(const char* key, int v) { Preferences p; p.begin("ks", false); p.putInt(key, v); p.end(); }
