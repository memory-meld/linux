#include "log.h"
#include "unwrap.h"

FILE *hememlogf;
FILE *timef;
FILE *statsf;
void log_init()
{
	hememlogf = UNWRAP(fopen("logs.txt", "w+"));
	UNWRAP(setvbuf(hememlogf, NULL, _IONBF, 0));

	timef = UNWRAP(fopen("times.txt", "w+"));
	UNWRAP(setvbuf(timef, NULL, _IONBF, 0));

	statsf = UNWRAP(fopen("stats.txt", "w+"));
	UNWRAP(setvbuf(statsf, NULL, _IONBF, 0));
}
