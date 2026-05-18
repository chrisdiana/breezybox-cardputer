# Ccleste Lua Optional App

This directory is an optional app payload. It is intentionally outside the
firmware build and can be copied, downloaded, or packaged into the device
filesystem later.

Install layout:

```text
/root/apps/ccleste/main.lua
/root/bin/ccleste
```

Run on device:

```sh
ccleste
```

or directly:

```sh
lua /root/apps/ccleste/main.lua
```

Controls:

- `a` / `d`: move
- `w`: look up / aim dash up
- `s`: aim dash down
- `z`: jump
- `x`: dash
- `r`: restart
- `q`: quit

This is the Lua-app packaging path for a downloaded optional app. It is not
part of the firmware image and does not depend on external ELF loading.
