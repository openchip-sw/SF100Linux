#pragma once

#ifndef _NETWORK_H
#define _NETWORK_H

#define NETWORK_MODE_NOT_REQUESTED (-1)

/* Handle --listen and --server before USB is initialized. */
int NetworkHandleMode(int argc, char* argv[]);

#endif
