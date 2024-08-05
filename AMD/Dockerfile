ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64
FROM quay.io/yshnaidm/amd_gpu_sources:el9-6.1.1 as sources

FROM quay.io/yshnaidm/driver-toolkit:${KERNEL_VERSION} as builder
ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64
RUN echo $KERNEL_VERSION
USER root
COPY --from=sources /amdgpu-drivers-source /amdgpu-drivers-source
WORKDIR /amdgpu-drivers-source
RUN ./amd/dkms/pre-build.sh ${KERNEL_VERSION}
RUN make TTM_NAME=amdttm SCHED_NAME=amd-sched -C /usr/src/kernels/${KERNEL_VERSION} M=/amdgpu-drivers-source
RUN ./amd/dkms/post-build.sh ${KERNEL_VERSION}


FROM registry.redhat.io/rhel9/rhel-bootc:9.4
ARG KERNEL_VERSION=5.14.0-427.16.1.el9_4.x86_64
RUN echo $KERNEL_VERSION

COPY --from=builder /amdgpu-drivers-source/amd/amdgpu/amdgpu.ko /opt/lib/modules/${KERNEL_VERSION}/amd/amdgpu/amdgpu.ko
COPY --from=builder /amdgpu-drivers-source/amd/amdkcl/amdkcl.ko /opt/lib/modules/${KERNEL_VERSION}/amd/amdkcl/amdkcl.ko
COPY --from=builder /amdgpu-drivers-source/amd/amdxcp/amdxcp.ko /opt/lib/modules/${KERNEL_VERSION}/amd/amdxcp/amdxcp.ko
COPY --from=builder /amdgpu-drivers-source/scheduler/amd-sched.ko /opt/lib/modules/${KERNEL_VERSION}/scheduler/amd-sched.ko
COPY --from=builder /amdgpu-drivers-source/ttm/amdttm.ko /opt/lib/modules/${KERNEL_VERSION}/ttm/amdttm.ko
COPY --from=builder /amdgpu-drivers-source/amddrm_buddy.ko /opt/lib/modules/${KERNEL_VERSION}/amddrm_buddy.ko
COPY --from=builder /amdgpu-drivers-source/amddrm_ttm_helper.ko /opt/lib/modules/${KERNEL_VERSION}/amddrm_ttm_helper.ko

RUN depmod $KERNEL_VERSION

COPY --chmod=744 ./rhsm-register /usr/local/bin/rhsm-register
COPY AMD/rocm.repo /etc/yum.repos.d/rocm.repo

RUN --mount=type=secret,id=RHSM_ORG,target=/run/secrets/RHSM_ORG \
    --mount=type=secret,id=RHSM_ACTIVATIONKEY,target=/run/secrets/RHSM_ACTIVATIONKEY \
    /usr/local/bin/rhsm-register \
    && dnf install -y podman wget\
    && wget https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm && rpm -ivh epel-release-latest-9.noarch.rpm \
    && crb enable \
    && dnf install -y rocm \
    && subscription-manager unregister \
    && dnf clean all \
    && rm /usr/local/bin/rhsm-register

COPY AMD/rocm.conf /etc/ld.so.conf.d/rocm.conf
RUN ldconfig
RUN export PATH=$PATH:/opt/rocm-6.0.2/bin
