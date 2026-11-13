# Build container

`docker/Dockerfile` is the build environment for opencamd: the camd daemon and the firmware image: the vendor MIPS
toolchain, the pruned GPL sources, host tools and Node. `compose.yaml` in the project root runs
it:

```sh
docker compose run --rm build          # tests + camd + UI + firmware image -> build/image/
docker compose run --rm build bash     # shell in the build environment
```

How the image is built, where the two Edimax inputs come from and how they are verified, what is
kept from them, and how CI and releases use the container: see
[`docs/building.md`](../docs/building.md).
