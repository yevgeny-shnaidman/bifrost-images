FROM fedora as source

COPY neuron.repo /etc/yum.repos.d/neuron.repo
RUN yum clean all
RUN  rpm --import https://yum.repos.neuron.amazonaws.com/GPG-PUB-KEY-AMAZON-AWS-NEURON.PUB
RUN yum install -y aws-neuronx-dkms-2.*

# DTK image
#FROM quay.io/yshnaidm/driver-toolkit:5.14.0-427.20.1.el9_4.x86_64 as build
FROM quay.io/yshnaidm/driver-toolkit:5.14.0-427.16.1.el9_4.x86_64 as build
USER root

COPY --from=source /usr/src/aws-neuronx-2.16.7.0 /aws-neuronx-2.16.7.0
WORKDIR /aws-neuronx-2.16.7.0

#fixing the neuron source code in order to enable compilation
RUN sed -i "s/\$(shell uname -r)/$(jq -r '.KERNEL_VERSION' /etc/driver-toolkit-release.json)/g" Makefile
RUN sed -i "s/KERNEL_VERSION(6, 4, 0)/KERNEL_VERSION(5, 14, 0)/g" neuron_cdev.c

RUN make

FROM registry.redhat.io/rhel9/rhel-bootc:9.4

#RUN mkdir -p /lib/modules/5.14.0-427.20.1.el9_4.x86_64/kernel/drivers/neuron
RUN mkdir -p /lib/modules/5.14.0-427.16.1.el9_4.x86_64/kernel/drivers/neuron
RUN echo "neuron" | tee -a /etc/modules-load.d/neuron.conf
RUN echo 'KERNEL=="neuron*", MODE="0666"' > /lib/udev/rules.d/neuron-udev.rules
#COPY --from=build /aws-neuronx-2.16.7.0/neuron.ko /lib/modules/5.14.0-427.20.1.el9_4.x86_64/kernel/drivers/neuron/neuron.ko
COPY --from=build /aws-neuronx-2.16.7.0/neuron.ko /lib/modules/5.14.0-427.16.1.el9_4.x86_64/kernel/drivers/neuron/neuron.ko

# preparing to install tools
COPY neuron.repo /etc/yum.repos.d/neuron.repo
RUN yum clean all
RUN  rpm --import https://yum.repos.neuron.amazonaws.com/GPG-PUB-KEY-AMAZON-AWS-NEURON.PUB

COPY --chmod=744 ./rhsm-register /usr/local/bin/rhsm-register
RUN --mount=type=secret,id=RHSM_ORG,target=/run/secrets/RHSM_ORG \
    --mount=type=secret,id=RHSM_ACTIVATIONKEY,target=/run/secrets/RHSM_ACTIVATIONKEY \
    /usr/local/bin/rhsm-register \
    && dnf install -y aws-neuronx-collectives-2.* aws-neuronx-runtime-lib-2.* aws-neuronx-tools-2.* \
    && subscription-manager unregister \
    && dnf clean all \
    && rm /usr/local/bin/rhsm-register

#RUN yum install aws-neuronx-collectives-2.* -y
#RUN yum install aws-neuronx-runtime-lib-2.* -y
#RUN yum install aws-neuronx-tools-2.* -y

ENV PATH="/opt/aws/neuron/bin:${PATH}"
#RUN depmod 5.14.0-427.20.1.el9_4.x86_64
RUN depmod 5.14.0-427.16.1.el9_4.x86_64
