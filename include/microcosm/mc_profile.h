/* Private profiling hooks. Normal builds leave all of it out, strings included. */
#ifndef MC_PROFILE_H
#define MC_PROFILE_H
#ifdef MC_PROFILE
enum { MC_P_SCENE, MC_P_SHADOW, MC_P_AO, MC_P_AO_FILTER, MC_P_BRIGHT,
       MC_P_RAYS, MC_P_BLOOM, MC_P_COMPOSITE, MC_P_TAA, MC_P_DOF, MC_P_COUNT };
void mc_profile_pass(int pass);
void mc_profile_cpu(int slot, double ms);
void mc_profile_startup(const char *label, double ms);
#define MC_PRECALC_START() double mc_precalc_start = mc_time_seconds()
#define MC_PRECALC_DONE(label) do { double now = mc_time_seconds(); mc_profile_startup(label, (now - mc_precalc_start) * 1000); mc_precalc_start = now; } while (0)
void mc_profile_pass_end(void);
#define MC_PASS(p) mc_profile_pass(p)
#define MC_PASS_END() mc_profile_pass_end()
#else
#define MC_PRECALC_START() ((void)0)
#define MC_PRECALC_DONE(label) ((void)0)
#define MC_PASS(p) ((void)0)
#define MC_PASS_END() ((void)0)
#endif
#endif
