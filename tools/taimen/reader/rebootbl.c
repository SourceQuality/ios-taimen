/* reboot(2) with a reason string ("bootloader": ABL stays in fastboot); busybox's reboot takes none. */
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	sync();
	return (int)syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	    LINUX_REBOOT_CMD_RESTART2, argc > 1 ? argv[1] : "bootloader");
}
