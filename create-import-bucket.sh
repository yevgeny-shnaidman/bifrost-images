#!/bin/bash

BUCKET=edgeinfo-ami-import 
#aws s3 mb s3://$BUCKET
printf '{ "Version":"2012-10-17", "Statement":[ { "Effect":"Allow", "Action":[ "s3:GetBucketLocation", "s3:GetObject", "s3:ListBucket" ], "Resource":[ "arn:aws:s3:::%s", "arn:aws:s3:::%s/*" ] }, { "Effect":"Allow", "Action":[ "ec2:ModifySnapshotAttribute", "ec2:CopySnapshot", "ec2:RegisterImage", "ec2:Describe*" ], "Resource":"*" } ] }' $BUCKET $BUCKET > role-policy.json
aws iam put-role-policy --role-name vmimport --policy-name vmimport-$BUCKET --policy-document file://role-policy.json
