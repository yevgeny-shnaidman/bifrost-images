#FROM quay.io/yshnaidm/rhel-bootc:9.4
FROM quay.io/kwozyman/magma:basicbootc

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
