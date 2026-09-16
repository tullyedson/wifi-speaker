#pragma once
#include <ArduinoJson.h>

namespace board_sensors {
// Called only by the main loop. Sampling never waits for a conversion to finish.
void begin();
void update();
void status(JsonObject output);
}
