Usage
=====

```
bcachefs-mount 0.1.0
Mount a bcachefs filesystem by its UUID

USAGE:
    bcachefs-mount [OPTIONS] <uuid> <mountpoint>

FLAGS:
    -h, --help       
            Prints help information

    -V, --version    
            Prints version information


OPTIONS:
    -o <options>                 
            Mount options [default: ]

ARGS:
    <uuid>          
            External UUID of the bcachefs filesystem

    <mountpoint>    
            Where the filesystem should be mounted
```

Build
=====

```sh
$ git submodule update --init --recursive
$ cargo build --release
```

Binary will be built in `target/release/bcachefs-mount`

Dependencies:

* rust
* blkid
* uuid
* liburcu
* libsodium
* zlib
* liblz4
* libzstd
