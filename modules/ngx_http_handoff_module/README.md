# XO NGINX Module
XO NGINX module implements XO for flow forwarding by offloading a TCP connection, rather than proxying between front- and backend connections.
## Building
### Building NGINX with XO module
```bash
$ git clone https://github.com/steven-chien/nginx
$ cd nginx
$ git checkout load-studies
$ ./auto/configure --add-module=./modules/ngx_http_handoff_module
$ make `nproc`
```
### Build CPU load monitoring tools
```bash
# cd modules/ngx_http_handoff_module/daemons
# gcc -g -Wall -o cpu_monitor.out cpu_monitor.c -lhiredis
cpu_monitor.c: In function ‘main’:
cpu_monitor.c:78:9: warning: unused variable ‘ret’ [-Wunused-variable]
   78 |     int ret;
      |         ^~~
# gcc -g -Wall -o monitor_backends.out monitor_backends.c -lhiredis
monitor_backends.c: In function ‘main’:
monitor_backends.c:35:9: warning: unused variable ‘ret’ [-Wunused-variable]
   35 |     int ret;
      |         ^~~
```
The code assumes 4 backends. Modify as appropriate.
### Configuration
Every backend and frontend should run NGINX using a config file, refer to `conf/xo.conf`. The configuration file specifies in the `http` block:
```nginx
    handoff_ifname enp8s0f0np0;
    handoff_freq 0; # 0 = round robin
    handoff_target 192.168.11.11 79;
    handoff_target 192.168.11.33 79;
    handoff_target 192.168.11.31 79;
    handoff_target 192.168.11.53 79;
```
After that, it defines the frontend server using the `handoff_out` directive. It means all connections received at this port will be offloaded to a `handoff_target`, where `79` is the port.
```nginx
    server {
        listen 80;
        location  / {
            proxy_no_cache 1;
            proxy_cache_bypass 1;
            proxy_http_version 1.1;
            handoff_out;
        }
    }
```
Finally, we define the backend servers using the `handoff_in` directive. `79` is the port where a backend server receives offload request and serve data to client.
```nginx
    server {
        listen 79;
        location  / {
            proxy_no_cache 1;
            proxy_cache_bypass 1;
            proxy_http_version 1.1;
            handoff_in;
        }
    }
```

## Running
### NIC Configuration
Refer to https://github.com/uoenoplab/xo/server for NIC configurations and to setup the eBPF program.
### CPU load monitoring
On every backend, run `cpu_monitor.out [ID]` in the backend (i.e. using `nohup`), where `ID` is determined by the order of specification in `xo.conf` for `handoff_target`.
On the front end, run `monitor_backends.out` in the background (i.e. using `nohup`).
### Running NGINX
Run NGNIX as usual.
```bash
# ./objs/nginx -c `pwd`/conf/xo.conf
```
The frontend will accept HTTP connection at port `80`.
