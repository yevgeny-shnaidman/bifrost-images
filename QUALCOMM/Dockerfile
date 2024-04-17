ARG KERNEL_VERSION=5.14.0-427.el9.x86_64

FROM quay.io/yshnaidm/qualcomm-sources:latest as source

# DTK image
FROM quay.io/fabiendupont/driver-toolkit:5.14.0-427.el9 as build
USER root
COPY --from=source /kernel-drivers-source /kernel-drivers-source

WORKDIR /kernel-drivers-source

RUN ./dkms_prebuild.sh 5.14.0-427.el9.x86_64 /usr/src/kernels/5.14.0-427.el9.x86_64
RUN make KERNELRELEASE=5.14.0-427.el9.x86_64


FROM quay.io/kwozyman/magma:basicbootc

RUN mkdir -p /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic
COPY --from=build /kernel-drivers-source/mhi/host/mhi.ko /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic/mhi.ko
COPY --from=build /kernel-drivers-source/qaic/qaic.ko /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic/qaic.ko
RUN depmod 5.14.0-427.el9.x86_64

COPY --from=source /fw-source /lib/firmware

COPY --from=source /sdk-source /sdk-source

COPY --chmod=744 ./rhsm-register /usr/local/bin/rhsm-register

RUN --mount=type=secret,id=RHSM_ORG,target=/run/secrets/RHSM_ORG \
    --mount=type=secret,id=RHSM_ACTIVATIONKEY,target=/run/secrets/RHSM_ACTIVATIONKEY \
    /usr/local/bin/rhsm-register \
    && dnf install -y podman wget yum gcc pciutils \
    && subscription-manager unregister \
    && dnf clean all \
    && rm /usr/local/bin/rhsm-register

WORKDIR /sdk-source
RUN ./x86_64/rpm/install.sh --container --custom_kmd
