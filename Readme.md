Bonjour for Spark Core
======================

This library is an adoption of EthernetBonjour for Arduino on Spark Core. 
Some code was rewritten to reflect platform specifics, but the logic and features are virtually the same.

Why another mDNS?
-----------------

Yes, I'm aware of [MDNS](https://github.com/krvarma/MDNS_SparkCore), but it is very limited. It doesn't really support service publishing, TXT records and other features of Bonjour. 

To support HomeKit in the future, we need full support of service publication.

Licence
-------

The original code was released under LGPL v3, so I don't have much choice here :)
