# nx-hbmenu for Switch 2, built in the Switch 2 devkit's image.
#   docker build --output out .           ->  out/hbmenu.nro
#   docker build --target build -t hbmenu-build .   (the build environment)
# DEVKIT_IMAGE is the devkit's image: build it from the devkit's Dockerfile, tagged
# ounce-devkit, or name another with --build-arg DEVKIT_IMAGE=...
ARG DEVKIT_IMAGE=ounce-devkit:latest
FROM ${DEVKIT_IMAGE} AS build
COPY . /src
WORKDIR /src
RUN make -j"$(nproc)" -f switch2/Makefile

FROM scratch
COPY --from=build /src/build/nro/nro/hbmenu.nro /
