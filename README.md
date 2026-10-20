samtools
========

This is meant to be a grab all for general purpose tools.

ipaddr
------

ipaddr is meant to be a more script friendly version of ifconfig or
ip. It should work for Linux, QNX, or BSD.

sntpdate
--------

Like ntpdate, but using sntp, which is simpler. With a -s argument it
is also a server, although it cheats and sends the time raw. Really
the server is meant for VM clients to get the time from the host.

myps
----

An attempt to simplify ps. It does not show kernel threads, and groups
similar apps together.
