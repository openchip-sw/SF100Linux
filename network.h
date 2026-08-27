#ifndef DPCMD_NETWORK_H
#define DPCMD_NETWORK_H

/* Returned when neither --listen nor --server was present. */
#define NETWORK_MODE_NOT_REQUESTED (-1)

/*
 * Handle the network-only command line modes before dpcmd opens USB devices.
 * The return value is NETWORK_MODE_NOT_REQUESTED for a normal local command,
 * otherwise it is the exit status that main() should return.
 */
int NetworkHandleMode(int argc, char* argv[]);

#endif
