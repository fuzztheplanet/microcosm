#ifndef PROFILE_H
#define PROFILE_H
#include <stdio.h>
int profile_init(FILE *csv);
void profile_frame(double time);
void profile_end(void);
void profile_wall(double ms);
void profile_drain(void);
void profile_close(void);
void mc_profile_cpu(int slot, double ms); /* slot 0: simulation, 1: preparation */
#endif
