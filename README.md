# vr_quiche
The project is dependent on following libraries: glib, uthash, gstreamer, quiche.

## Run
- put all files under quiche/example, then make

- params: host port use_dgram(0:stream, 1:dgram)

- server/sender：

```bash
./gserver2 127.0.0.1 23333 0 2>srv.log
```

- client/receiver：

```bash
./gclient2 127.0.0.1 23333 0 2>cli.log
```


## Update 07/15
DTP-like scheduler is implemented when sending streams (to replace Round-robin). Note that to use DTP-like scheduler, the "urgency" from the original API needs to be set as the same. Also, streams must be incremental.
