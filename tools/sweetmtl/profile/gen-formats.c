/*
 * gen-formats: what freedreno does with each VkFormat on this device (sg_format_features), one line per format:
 * "<VkFormat value> <SG_FEAT_* bits>". make-profile.py turns the lines into the device profile's format table. Run on
 * the PC under freedreno drm-shim with FD_GPU_ID=540 (the answers come from the driver's tables, nothing executes).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "sg.h"

int main(void)
{
	struct sg_device *dev = sg_device_open();
	if (!dev) {
		fprintf(stderr, "gen-formats: %s\n", sg_last_error());
		return 1;
	}
	printf("# %s gpu %u\n", sg_device_name(dev), sg_device_gpu_id(dev));
	static const unsigned extra[] = {1000340000, 1000340001, 1000470000, 1000470001};
	for (unsigned vk = 1; vk <= 184; vk++)
		printf("%u %u\n", vk, sg_format_features(dev, vk));
	for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
		printf("%u %u\n", extra[i], sg_format_features(dev, extra[i]));
	sg_device_close(dev);
	return 0;
}
