/**
 * @file translate_app.h
 * @brief Live Spanish/English translation at a table: microphone to the bridge, phrases back.
 */
#pragma once
#include "app.h"

extern const App translate_app;

namespace translate {
void set_listening(bool on);   ///< start/stop streaming the microphone
void toggle_flip();            ///< turn the far panel upside down for the person opposite
void register_console();
}
