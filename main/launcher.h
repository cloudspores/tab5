// Home screen and app switching.
#pragma once
#include "app.h"

namespace launcher {
void init();                         // builds the home screen; call after display start
void add(const App *app);            // register in card order
void home();                         // show the home screen
void open(const char *id);           // show an app by id
const char *current();               // id of the visible app, or "home"
void refresh();                      // re-read card status texts
}
