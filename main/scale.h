// Reading scale: some meters report the register 10x or 100x too big (the counter unit differs between models).
// The receiver multiplies each decoded reading by a factor: x1, x0.1, x0.01 or x0.001.
//  - a global default for all meters        (NVS "scale_all",  command: scale all <factor>)
//  - an optional override for one meter     (NVS "s_<id8>",    command: scale <meter id> <factor> | default)
// Values are stored as factor * 1000.
#pragma once
struct ScaleEnt { char id[16]; int32_t own; };   // own = 0: follow the global default
static ScaleEnt scales[64];
static int nscales = 0;
static int32_t scale_all_m = 0;                    // 0 = not loaded yet

static int32_t scale_global() {
  if (!scale_all_m) { int32_t v = 1000; if (nvs_get_i32(nvs, "scale_all", &v) != ESP_OK || v <= 0) v = 1000; scale_all_m = v; }
  return scale_all_m;
}
static ScaleEnt *scale_ent(const char *id) {
  for (int i = 0; i < nscales; i++) if (!strcmp(scales[i].id, id)) return &scales[i];
  char k[12]; snprintf(k, sizeof k, "s_%.8s", id);
  int32_t v = 0;
  if (nvs_get_i32(nvs, k, &v) != ESP_OK || v < 0) v = 0;
  ScaleEnt *e = &scales[nscales < 64 ? nscales++ : 63];
  strncpy(e->id, id, 15); e->id[15] = 0; e->own = v;
  return e;
}
static int32_t scale_own(const char *id) { return scale_ent(id)->own; }
static int32_t scale_milli(const char *id) { int32_t o = scale_own(id); return o ? o : scale_global(); }
static double scale_of(const char *id) { return scale_milli(id) / 1000.0; }
static void scale_set_own(const char *id, int32_t milli) {   // 0 = back to the global default
  scale_ent(id)->own = milli;
  char k[12]; snprintf(k, sizeof k, "s_%.8s", id);
  if (milli) nvs_set_i32(nvs, k, milli); else nvs_erase_key(nvs, k);
  nvs_commit(nvs);
}
static void scale_set_global(int32_t milli) {
  scale_all_m = milli;
  if (milli == 1000) nvs_erase_key(nvs, "scale_all"); else nvs_set_i32(nvs, "scale_all", milli);
  nvs_commit(nvs);
}
static int32_t scale_factor_parse(const char *f) {        // "0.1" -> 100 ; invalid -> 0
  int32_t m = (int32_t)lround(atof(f) * 1000);
  return (m == 1000 || m == 100 || m == 10 || m == 1) ? m : 0;
}
