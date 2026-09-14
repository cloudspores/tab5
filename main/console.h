/**
 * @file console.h
 * @brief Line-oriented command console on the USB serial port.
 *
 * Every app registers the commands it understands; the console parses "verb [argument]" lines and
 * dispatches them. It exists for testing without touching the screen and for the bridge/knobs
 * later. Reads are polled on the non-driver USB VFS, which avoids the USB driver's interrupt storm
 * seen on this board.
 */
#pragma once

namespace console {

/** Handler receives the argument text ("" when none) and its integer value (0 when none). */
using Handler = void (*)(const char *arg, int value);

/** Register a verb. Up to 32 commands; later registrations of the same verb replace earlier ones. */
void add(const char *verb, Handler h, const char *help);

/** Start the console task. */
void start();

}
