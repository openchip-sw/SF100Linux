# SF100Linux
Linux software for Dediprog SF100 and SF600 SPI flash programmers

## Building
To compile the project, first install required dependencies:
  - libusb-1.0
  - libxml-2.0
  - pkg-config - won't link to libusb without this package

Change to the directory where the sources are located and build using make:
```bash
$ cd SF100Linux
$ make
```
or use
```bash
$ make install
```

The resulting binary should be called `dpcmd` and located in the root of the
source tree. There is no install target at the moment.

## Usage
Most basic usage is writing a whole image file to a flash chip:
```bash
$ ./dpcmd --auto image.bin --verify
```

This will automatically detect the chip, read out the chip contents, replace
the ones that differ and perform a read and verification after writing.

For more advanced usage see `dpcmd --help`.

## Minimal network mode

The computer connected to the programmer can expose `dpcmd` to one network
client at a time:

```bash
$ ./dpcmd --listen 0.0.0.0:9000
```

From another computer, add `--server HOST:PORT` to a normal command. The BIN
image is read on the client, transferred to the server, and programmed by the
server's locally connected programmer:

```bash
$ ./dpcmd --server 192.168.1.50:9000 --auto image.bin --verify
```

Using only a port (for example, `--listen 9000`) binds the server to
`127.0.0.1`. This minimal protocol has no authentication or encryption, so it
should only be exposed on a trusted network. It supports one input BIN image
per command and intentionally does not allow remote reads, raw instructions,
firmware updates, or server-side log paths.

To restrict a server to one programmer, specify its selector when starting the
listener. Every client request must supply the same selector type and value:

```bash
# Server
./dpcmd --listen 0.0.0.0:9000 --device-SN SF129624
# Client
./dpcmd --server SERVER_IP:9000 --device-SN SF129624 --detect
```

Alternatively, use `--device 2` on both server and client. A missing, mismatched,
duplicate, or mixed selector is rejected with a nonzero exit status before the
local programmer command starts. A listener without a selector remains unrestricted.
Serial IDs are preferable because device numbering can change after reconnection.
This restriction checks the requested target; the current USB initialization still
opens all attached programmers before local command options are parsed.
