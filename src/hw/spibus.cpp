#include "spibus.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board_pins.h"

namespace spibus {
namespace {

SemaphoreHandle_t gMux = nullptr;

// LilyGO's factory firmware raises every CS on the shared bus before and after
// each transaction. Leaving an inactive device selected (or floating) lets it
// drive MISO into the e-paper's BUSY wait and hang the panel forever.
void releaseAllCs() {
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS, HIGH);
    digitalWrite(BOARD_EPD_CS, HIGH);
}

}  // namespace

void begin() {
    pinMode(BOARD_LORA_CS, OUTPUT);
    pinMode(BOARD_SD_CS, OUTPUT);
    pinMode(BOARD_EPD_CS, OUTPUT);
    releaseAllCs();

    if (gMux) return;
    gMux = xSemaphoreCreateRecursiveMutex();
}

Lock::Lock() {
    if (gMux) xSemaphoreTakeRecursive(gMux, portMAX_DELAY);
    releaseAllCs();
}

Lock::~Lock() {
    releaseAllCs();
    if (gMux) xSemaphoreGiveRecursive(gMux);
}

}  // namespace spibus
