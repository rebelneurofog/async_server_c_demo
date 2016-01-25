# async_server_c_demo
Async network demo as simple as possible

## Build
  make

## Usage
After build run following to start service:
    ./network_demo

The server will run on 0.0.0.0:4455

Run multiple telnet clients on several terminals:
    telnet 127.0.0.1 4455

Now you may send one-line messages to be delivered to all clients.
Also server sends broadcast message to all clients
