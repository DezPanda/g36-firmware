/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36-blkpg - tell the kernel about a partition change on a disk that's in use.
 *
 * sfdisk can't re-read the card's partition table while the OS partition on it
 * is mounted, so new or resized partitions would only appear after a reboot.
 * This asks the kernel to add, resize or delete one partition directly.
 *
 *   g36-blkpg add    /dev/mmcblk0 5 <start-sector> <length-sectors>
 *   g36-blkpg resize /dev/mmcblk0 4 <start-sector> <length-sectors>
 *   g36-blkpg delete /dev/mmcblk0 5
 *
 * Sectors are 512 bytes.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
   struct blkpg_partition part;
   struct blkpg_ioctl_arg arg;
   int fd, op;

   if (argc < 4 || (strcmp(argv[1], "delete") && argc < 6))
   {
      fprintf(stderr, "usage: %s add|resize <disk> <partno> <start> <length>\n"
                      "       %s delete <disk> <partno>\n", argv[0], argv[0]);
      return 2;
   }
   if (!strcmp(argv[1], "add"))
      op = BLKPG_ADD_PARTITION;
   else if (!strcmp(argv[1], "resize"))
      op = BLKPG_RESIZE_PARTITION;
   else if (!strcmp(argv[1], "delete"))
      op = BLKPG_DEL_PARTITION;
   else
   {
      fprintf(stderr, "%s: unknown operation '%s'\n", argv[0], argv[1]);
      return 2;
   }

   memset(&part, 0, sizeof(part));
   part.pno = atoi(argv[3]);
   if (op != BLKPG_DEL_PARTITION)
   {
      part.start  = strtoll(argv[4], NULL, 10) * 512LL;
      part.length = strtoll(argv[5], NULL, 10) * 512LL;
   }

   memset(&arg, 0, sizeof(arg));
   arg.op      = op;
   arg.datalen = sizeof(part);
   arg.data    = &part;

   if ((fd = open(argv[2], O_RDONLY)) < 0)
   {
      fprintf(stderr, "%s: %s: %s\n", argv[0], argv[2], strerror(errno));
      return 1;
   }
   if (ioctl(fd, BLKPG, &arg) < 0)
   {
      fprintf(stderr, "%s: %s partition %d: %s\n", argv[0], argv[1], part.pno, strerror(errno));
      close(fd);
      return 1;
   }
   close(fd);
   return 0;
}
