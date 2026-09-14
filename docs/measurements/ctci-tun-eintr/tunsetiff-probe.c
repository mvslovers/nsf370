#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
int main(void){
    struct ifreq ifr; int fd, rc, i;
    for (i = 0; i < 3; i++) {
        fd = open("/dev/net/tun", O_RDWR);
        if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }
        memset(&ifr, 0, sizeof ifr);
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
        errno = 0;
        rc = ioctl(fd, TUNSETIFF, (void *)&ifr);
        printf("try %d: TUNSETIFF rc=%d errno=%d (%s) name='%s'\n",
               i, rc, errno, rc<0?strerror(errno):"ok", ifr.ifr_name);
        close(fd);
    }
    return 0;
}

/*
 * tunsetiff-probe.c -- what does an UNPRIVILEGED TUNSETIFF return on this box?
 *
 * The one measurement that decides where Hercules' EINTR comes from.  If this
 * prints EPERM, the ioctl is not the source and Hercules necessarily takes its
 * hercifc fallback, so the EINTR is in the parent's select()/read() handshake.
 * If it printed EINTR, the kernel would be the whole story and hercifc would be
 * irrelevant.  Measured 2026-09-03 on mvsdev: EPERM, 3 of 3.
 *
 *   scp this to the host, then:  gcc -o tuntest tunsetiff-probe.c && ./tuntest
 *
 * Run it as the SAME user Hercules runs as.  Run as root it would succeed and
 * tell you nothing.
 */
