# G36 drive mode for the factory usbdevice script (sourced from /etc/usbdevice.d/).
# Shares both exFAT partitions with a computer: LUN 0 = G36-GAMES, LUN 1 = G36-EMU.
# g36-drivemode unmounts them before sharing and checks + remounts them afterwards,
# so the stock image-file handling, fsck and auto-mount are replaced here.

ums_prepare()
{
	mkdir -p lun.1
	for lun in lun.0 lun.1; do
		echo > $lun/file
		echo 1 > $lun/removable
		echo 0 > $lun/ro
	done
	echo /dev/mmcblk0p5 > lun.0/file
	echo /dev/mmcblk0p4 > lun.1/file
}

ums_start()
{
	:
}

ums_stop()
{
	echo > lun.0/file
	echo > lun.1/file 2>/dev/null
	# LUN 1 was added here, so remove it here (the function can't be removed while it exists)
	rmdir lun.1 2>/dev/null
}
