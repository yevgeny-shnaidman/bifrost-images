#!/bin/bash
#
usage(){
    cat << EOF
    upload-ami.sh - Upload AMI raw file to AWS and convert it to AMI Image ready to use

    Usage:
    upload-ami.sh <local_raw_ami> <ami_name> <intermediate_s3_bucket_name>

    Example
    upload_ami.sh nvidia-bootc/build/image/disk.raw javipolo-ilab-v023 javipolo
EOF
exit 1
}

if [[ $# -lt 3 ]]; then
    usage
fi

LOCAL_AMI="$1"
AMI_NAME="$2"
BUCKET="$3"

RAW_AMI=ami_raw_tmp.ami
TMPFILE=$(mktemp)

set -e # Halt on error

aws s3 cp "$LOCAL_AMI" s3://$BUCKET/$RAW_AMI && \
printf '{ "Description": "my-image", "Format": "raw", "UserBucket": { "S3Bucket": "%s", "S3Key": "%s" } }' $BUCKET $RAW_AMI > $TMPFILE

echo $TMPFILE

echo "YEV"

echo Importing Snapshot
TASK_ID=$(aws ec2 import-snapshot --disk-container file://$TMPFILE | jq -r .ImportTaskId)
echo "import task id is $TASK_ID"
while aws ec2 describe-import-snapshot-tasks --filters Name=task-state,Values=active | jq -r '.ImportSnapshotTasks[].ImportTaskId' | grep -qx $TASK_ID; do
    echo -n .; sleep 1
done; echo

echo "YEV2, import snapshot task id $TASK_ID"

SNAPSHOT_ID=$(aws ec2 describe-snapshots | jq -r '.Snapshots[] | select(.Description | contains("'${TASK_ID}'")) | .SnapshotId')

echo "YEV 3, SNAPSHOT_ID $SNAPSHOT_ID"

aws ec2 create-tags --resources $SNAPSHOT_ID --tags Key=Name,Value="$AMI_NAME"

echo Registering AMI
AMI_ID=$(aws ec2 register-image  \
    --name "$AMI_NAME" \
    --description "$AMI_NAME" \
    --architecture x86_64 \
    --root-device-name /dev/sda1 \
    --block-device-mappings "DeviceName=/dev/sda1,Ebs={SnapshotId=${SNAPSHOT_ID}}" \
    --virtualization-type hvm \
    --ena-support \
    | jq -r .ImageId)

echo "YEV 4, AMI ID $AMI_ID"

aws ec2 create-tags --resources $AMI_ID --tags Key=Name,Value="$AMI_NAME"
aws s3 rm s3://$BUCKET/$RAW_AMI
rm -f "$TMPFILE"

echo "Created AMI $AMI_ID with name $AMI_NAME"


