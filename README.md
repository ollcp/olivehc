# OliveHC - Olive HTTP Cache #

OliveHC is a simple, reliable and high-performance cache.

This project is designed and used by Baidu CDN team, but also applied by several other departments in company.

It only works on Linux/x86_64 by now.


## Easy to Use ##

The function is simple. The configuration is simple and can be modified by reloading.

Install:

    make            # "output/" will be created

After creating a simple configure file, as the next section, you can start up:

    ./olivehc       # "olivehc.pid" will be created

Quit:

    echo quit | nc 127.1 5210

Reload configure:

    echo reload | nc 127.1 5210

Running status:

    echo status | nc 127.1 5210


## Configuration File ##

See `olivehc.conf.sample`.

Only `device` and `server` are required. Use `device` to assign some files for storage, and `server` to assign some listen ports.

Other commands are optional, and their default values are given in `olivehc.conf.sample`.


## Multi Server ##

You can assign more than one `server`, and each can be independent configured, like capacity, timeouts, and so on.


## HTTP Interface ##

More suitable for WebServer cache (compared with memcached protocol).

GET/HEAD, PUT/POST, and DELETE methods are supported.

Range read is supported, but no range store.

The only diffirence between PUT and POST is that, if the item exists, PUT will replace it, while POST just give up.

When storing an item, besides its body, all HTTP request headers (except `Connection`) are stored too, as the response headers for the following GET/HEAD requests.


## Item Management ##

OliveHC manages the items itself, while not use the disk file system, in order to avoid frequent `open`/`close`/`unlink` syscall, for better performance.

Item meta data stay in memory while the process is working. They will be dumped onto store devices only when OliveHC quits, for persistence. If OliveHC quits abnormally, all data lose.

Each item meta takes about 88 bytes, so 100 million items takes about 9GB memory.


## Store Device ##

OliveHC does not support multi-level storage for heat stratification, but only file storage, and using operating system cache for heat management. For the [reason](https://www.varnish-cache.org/trac/wiki/ArchitectNotes).

Storage based on file, which supports 2 kinds: normal file and block device file(like disk or disk patition). Block device file is suggested, which makes sure that items are physically contiguous on disk. You can use `/dev/ramdisk` for pure memory cache.

Because of the limit 0x4020010000 (about 270GB) of `sendfile`, the size of store device must be less that this value. If you want to use a disk which is too large, you can partition it into several small partitions.


## Multi Thread ##

OliveHC has one master thread and several worker threads.

Master thread processes requests and manages items. Worker threads only make disk IO, to take advantage of multiple disks, and avoid the influence of blocked IO operation.

As a result, though multi thread, besides the simple communication between master and each worker by pipe, there is no other synchronization(like lock) needed.


## Expire ##

The expire time of each item is set as: use the `expire_force` value of its server's configuration, if set (default is not set); otherwise use the `Cache-Control` or `Expires` header value in PUT/POST requset; otherwise use the `expire_default` value(default is 3days) of its server's configuration.

If the space usage of a server exceed its capacity, its items are expired in accordance with LRU.

In order to avoid the rushing of a large number of long tail cold items (the rushing includes crowding out hot items, and disk writing operation), OliveHC can filter cold items. If turing on this feature, for the first store request of each item, we only record its URL, but not store its data; if a second store request comes again, before the URL expired, we store the item really.

You can set the threashold of space usage percent at which enables the feature, and the limit of cold URLS number.


## Space Division ##

Fixed length division is used when allocating space for items. The method is that, inserting 3 equal difference values bewteen each two equal ratio values. Like:

    ... [4] 5 6 7 [8] 10 12 14 [16] ...

This method brings about 8% space waste.

Besides, if the size of items covers a large range, it makes space fragment.


## Working with Nginx ##

Because OliveHC does not have the ability to access the origin when request missing, so we need the WebServer to fetch the item from origin.

We use Nginx as WebServer, and add a filter module `jstore`, to store the missing item to OliveHC, by subrequest.

See `nginx-jstore\` for the code and usage.
