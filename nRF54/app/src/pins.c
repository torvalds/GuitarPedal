/*
 * The two pins that are not GPIOs until somebody says so.
 *
 * P1.02 and P1.03 carry the i2s data lines, and they are also NFC1 and
 * NFC2.  NFCT.PADCONFIG.ENABLE resets to 1, so out of every reset those
 * two pads are an NFC antenna and the i2s link has half of it missing.
 *
 * The devicetree has a property for this - nfct-pins-as-gpios - and it
 * does not work on this build.  It reaches Nordic's own startup code as
 * a compile definition, and the register write it enables sits inside a
 * `defined(__ARM_FEATURE_CMSE)` guard: a secure image built without
 * TrustZone, which is what this is, compiles it out.  So the property
 * looks right, builds clean, appears in the generated devicetree, and
 * changes nothing.
 *
 * Do it here instead, before any driver could want the pins.
 */

#include <zephyr/init.h>
#include <hal/nrf_nfct.h>

static int free_nfc_pins(void)
{
	nrf_nfct_pad_config_enable_set(NRF_NFCT, false);
	return 0;
}

SYS_INIT(free_nfc_pins, PRE_KERNEL_1, 0);
