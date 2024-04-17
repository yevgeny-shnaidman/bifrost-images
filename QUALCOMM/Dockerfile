ARG KERNEL_VERSION=5.14.0-427.el9.x86_64

# DTK image
FROM quay.io/fabiendupont/driver-toolkit:5.14.0-427.el9 as build
ARG VENDOR=QUALCOMM
USER root
COPY $VENDOR/kernel-drivers /kernel-drivers

WORKDIR /kernel-drivers

RUN ./dkms_prebuild.sh 5.14.0-427.el9.x86_64 /usr/src/kernels/5.14.0-427.el9.x86_64
RUN make KERNELRELEASE=5.14.0-427.el9.x86_64


FROM quay.io/kwozyman/magma:basicbootc

ARG VENDOR=QUALCOMM

RUN mkdir -p /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic
COPY --from=build /kernel-drivers/mhi/host/mhi.ko /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic/mhi.ko
COPY --from=build /kernel-drivers/qaic/qaic.ko /lib/modules/5.14.0-427.el9.x86_64/kernel/qaic/qaic.ko
RUN depmod 5.14.0-427.el9.x86_64

COPY $VENDOR/qaic-platform-sdk-1.14.2.0 /qaic-platform-sdk-1.14.2.0

COPY --chmod=744 ./rhsm-register /usr/local/bin/rhsm-register

RUN --mount=type=secret,id=RHSM_ORG,target=/run/secrets/RHSM_ORG \
    --mount=type=secret,id=RHSM_ACTIVATIONKEY,target=/run/secrets/RHSM_ACTIVATIONKEY \
    /usr/local/bin/rhsm-register \
    && dnf install -y podman wget yum gcc pciutils \
    && subscription-manager unregister \
    && dnf clean all \
    && rm /usr/local/bin/rhsm-register

WORKDIR /qaic-platform-sdk-1.14.2.0
RUN ./x86_64/rpm/install.sh --container --custom_kmd
