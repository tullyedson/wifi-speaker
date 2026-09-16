#pragma once
#include <ArduinoJson.h>

namespace board_sensors {
struct Presence { bool available = false; bool detected = false; };
// Called only by the main loop. Sampling never waits for a conversion to finish.
void begin();
void update();
void status(JsonObject output);
Presence presence();
}
