# Firmware signing key

Generate this device's private OTA signing key before the first sysbuild:

```shell
mkdir -p dog-door/keys
imgtool keygen -k dog-door/keys/dog-door-signing.pem -t ecdsa-p256
```

`dog-door-signing.pem` is ignored by Git. Back it up securely: MCUboot will
only accept OTA images signed by this key. If the key is lost, install a new
bootloader and application over serial with a newly generated key.
