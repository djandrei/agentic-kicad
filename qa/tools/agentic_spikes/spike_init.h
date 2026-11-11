// Agentic-KiCad spike support.

#ifndef SPIKE_INIT_H
#define SPIKE_INIT_H

/// Initialise wx and a mock PGM_BASE with a private config dir, as the QA test modules do.
bool SpikeInit( int aArgc, char** aArgv );

/// Same, with a GUI wxApp (GTK initialised) so windows and canvases can be created.
bool SpikeInitGui( int aArgc, char** aArgv );

/// Number of wx asserts raised since SpikeInit (console spikes).
extern int g_spikeAsserts;

/// Monotonic clock in milliseconds.
double SpikeNowMs();

#endif
