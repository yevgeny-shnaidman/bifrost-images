#!/usr/bin/env bash

set -e

RHSM_ORG_FILE=${RHSM_ORG_FILE:-'.rhsm_org'}
RHSM_ACTIVATIONKEY_FILE=${RHSM_ACTIVATIONKEY_FILE:-'.rhsm_activationkey'}

VENDOR=$1
BIFROST_IMAGE=$2

echo $VENDOR
echo $BIFROST_IMAGE

podman build -f ${VENDOR}/Dockerfile \
    --secret id=RHSM_ORG,src=${RHSM_ORG_FILE} \
	--secret id=RHSM_ACTIVATIONKEY,src=${RHSM_ACTIVATIONKEY_FILE} \
    -t ${BIFROST_IMAGE} .
