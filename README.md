# vr_quiche

# Run
params: host port use_dgram(0:stream, 1:dgram)

./gserver2 127.0.0.1 23333 0 2>srv.log
./gclient2 127.0.0.1 23333 0 2>cli.log
