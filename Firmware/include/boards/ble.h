//
// The minimal board with its radio, carrying the radio's firmware.
//
// The pins are minimal's.  What this adds is the nRF54's image, built
// from nRF54/ and embedded in this one, and the code to write it into
// the radio's RRAM over SWD when the radio is not running it - so one
// .uf2 updates both chips.  Building it needs the Nordic SDK
// ('make -C nRF54 fetch'); minimal does not.
//
// Both boards probe for a radio and work with or without one.  Only
// this one can rewrite it.
//
#include "minimal.h"

#define NRF54_IMAGE		1
