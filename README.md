# bifrost-images

This repo contains instructions to build BIFROST image for AMD and QUALCOMM AIs.

Instructions:
1) obtain auth tokens for RedHat Subscriptions (.rhsm_activationkey and .rhsm_org)
2) put those file inside the root directory (where container.sh script is present)
3) run: container-image.sh <vendor> <resulting-bifrost-image>
   <vendor>: which vendor image you want to build ( AMD or QUALCOMM). vendor must be equal to one of the directories
   <resulting-bifrost-image>: the name for the output bifrost image
   example: container-image.sh AMD quay.io/yshnaidm/amd-bifrost
