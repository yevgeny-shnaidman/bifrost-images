ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64

FROM quay.io/yshnaidm/qualcomm-sources:new-sources as source
ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64

# DTK image
FROM quay.io/yshnaidm/driver-toolkit:$KERNEL_VERSION as build
ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64
RUN echo $KERNEL_VERSION
USER root
COPY --from=source /kernel-drivers-source /kernel-drivers-source

WORKDIR /kernel-drivers-source

RUN ./dkms_prebuild.sh $KERNEL_VERSION /usr/src/kernels/$KERNEL_VERSION
RUN make KERNELRELEASE=$KERNEL_VERSION


FROM registry.redhat.io/rhel9/rhel-bootc:9.4
ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64
RUN echo $KERNEL_VERSION

RUN mkdir -p /lib/modules/$KERNEL_VERSION/kernel/qaic
COPY --from=build /kernel-drivers-source/mhi/host/mhi.ko /lib/modules/$KERNEL_VERSION/kernel/drivers/bus/mhi/host/mhi.ko
COPY --from=build /kernel-drivers-source/qaic/qaic.ko /lib/modules/$KERNEL_VERSION/kernel/qaic/qaic.ko
RUN rm -f /lib/modules/$KERNEL_VERSION/kernel/drivers/bus/mhi/host/mhi.ko.xz
RUN depmod $KERNEL_VERSION

COPY --from=source /firmware /lib/firmware

COPY --from=source /sdk-source /sdk-source

COPY --chmod=744 ./rhsm-register /usr/local/bin/rhsm-register

RUN --mount=type=secret,id=RHSM_ORG,target=/run/secrets/RHSM_ORG \
    --mount=type=secret,id=RHSM_ACTIVATIONKEY,target=/run/secrets/RHSM_ACTIVATIONKEY \
    /usr/local/bin/rhsm-register \
    && dnf install -y podman wget yum gcc pciutils \
    && /sdk-source/x86_64/rpm/install.sh --custom_kmd \
    && subscription-manager unregister \
    && dnf clean all \
    && rm /usr/local/bin/rhsm-register

WORKDIR /sdk-source
RUN ./x86_64/rpm/install.sh --custom_kmd

## use /opt/qti-aic/tools/qaic-util to test the kernel driver for A100
