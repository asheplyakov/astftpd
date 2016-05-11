=======
astftpd
=======

astftpd is a special purpose read only TFTP server. Unlike other tftp 
servers it uses IO multiplexing instead of process/thread per a client
approach. As a result it can serve 400+ clients concurrently on a very
moderate hardware.

Intended usage
--------------

Serving a handful of relatively small files to a lot of concurrent clients.
For instance booting a cluster of 400+ similar/same machines via the network.
Forking/threading TFTP servers are unable to handle such a load so PXE firmware
times out, thus rebooting the cluster requires (a complicated) orchestration.

Building and running
--------------------

::

  make
  sudo setcap cap_net_bind_service=+ep ./astftpd
  ./astftpd /path/to/vmlinuz /path/to/initramfs

Limitations
-----------

* astftpd runs only on Linux.
* The list of files being served should be explicitly specified on startup.
  Linux does not provide IO multiplexing with ordinary files, hence the hack.
* Only IPv4 is supported.
* Multicast TFTP is not implemented (no PXE firmware I know of supports it).
* ``blksize`` is the only TFTP option astftpd knowns of.

