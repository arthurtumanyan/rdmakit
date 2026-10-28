/*
 * coord.h - coordinated runs of several load generators ("kvpace barrier" and --coordinator).
 *
 * A coordinator waits for a fixed number of generators, starts them together and merges their results:
 *
 *   kvpace barrier --listen [ADDR:]PORT --count N --out MERGED.json [--timeout SEC]
 *   kvpace --coordinator HOST:PORT ...      (on every generator host)
 *
 * Protocol (text lines over TCP, one connection per generator):
 *   generator -> coordinator   "READY\n"        connected, preloaded, every worker ready
 *   coordinator -> generator   "GO\n"           sent to all generators once N are ready
 *   generator -> coordinator   "RESULT <n>\n" followed by n bytes: the JSON result of the run
 *                              "FAILED\n"       the run failed
 * The start message replaces a start time, so the generators' clocks need not be synchronized; they start
 * within the time it takes the coordinator to write N short messages and the network to deliver them.
 * The coordinator stores every result next to MERGED.json (MERGED-1.json, ...) and merges them as
 * "kvpace merge" does, which also reports how long the inputs measured together.
 */
#ifndef PACE_COORD_H
#define PACE_COORD_H

#include <stddef.h>

/* "kvpace barrier": arguments after the subcommand name (argv[0] is "barrier"). Returns the exit status. */
int pace_barrier_main(int argc, char **argv);

typedef struct pace_barrier pace_barrier;

/* Opens the coordinator's listening socket (port "0" picks a free port). Returns NULL with a message in err. */
pace_barrier *pace_barrier_open(const char *addr, const char *port, unsigned count, char *err, size_t errlen);

/* Port the coordinator listens on. */
int pace_barrier_port(const pace_barrier *b);

/*
 * Waits for count generators, starts them, collects and merges their results into out. timeout_s bounds
 * the whole run (0 = no bound). Returns 0, or -1 with a message on standard error.
 */
int pace_barrier_run(pace_barrier *b, const char *out, double timeout_s);

void pace_barrier_close(pace_barrier *b);

typedef struct pace_coord pace_coord;

/* Connects a generator to the coordinator at "HOST:PORT". Returns NULL with a message in err. */
pace_coord *pace_coord_connect(const char *hostport, char *err, size_t errlen);

/* Start hook of the runner (pace_config.start_hook, arg = the pace_coord): reports ready, waits for GO. */
int pace_coord_start(void *arg);

/* Sends the JSON result of the run. Returns 0 or -1. */
int pace_coord_result(pace_coord *c, const char *json, size_t len);

/* Reports a failed run. */
void pace_coord_failed(pace_coord *c);

void pace_coord_close(pace_coord *c);

#endif
